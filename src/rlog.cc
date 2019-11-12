#include "rlog.h"

#include <errno.h>
#include <unistd.h>//access, getpid
#include <assert.h>//assert
#include <stdarg.h>//va_list
#include <sys/stat.h>//mkdir
#include <sys/syscall.h>//system call

#define MEM_USE_LIMIT (3u * 1024 * 1024 * 1024)//3GB
#define LOG_USE_LIMIT (1u * 1024 * 1024 * 1024)//1GB
#define LOG_LEN_LIMIT (4 * 1024)//4K
#define RELOG_THRESOLD 5
#define BUFF_WAIT_TIME 1

// 这里为什么要换成syscall
pid_t gettid()
{
    return syscall(__NR_gettid);
}

pthread_mutex_t ring_log::_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ring_log::_cond = PTHREAD_COND_INITIALIZER;

ring_log* ring_log::_ins = nullptr;
// PTHREAD_ONCE_INIT定义为0
pthread_once_t ring_log::_once = PTHREAD_ONCE_INIT;
// 一个ring buffer 30MB
uint32_t ring_log::_one_buff_len = 30*1024*1024;

ring_log::ring_log():
    _buff_cnt(3),
    _curr_buf(nullptr),
    _prst_buf(nullptr),
    _fp(nullptr),
    _log_cnt(0),
    _env_ok(false),
    _level(INFO),
    _lst_lts(0),
    _tm()
{
    //create double linked list
    cell_buffer* head = new cell_buffer(_one_buff_len);
    if (!head)
    {
        fprintf(stderr, "no space to allocate cell_buffer\n");
        exit(1);
    }
    cell_buffer* current;
    cell_buffer* prev = head;
    // 默认3个ring buffer
    for (int i = 1;i < _buff_cnt; ++i)
    {
        current = new cell_buffer(_one_buff_len);
        if (!current)
        {
            fprintf(stderr, "no space to allocate cell_buffer\n");
            exit(1);
        }
        current->prev = prev;
        prev->next = current;
        prev = current;
    }
    prev->next = head;
    head->prev = prev;

    _curr_buf = head;
    _prst_buf = head;

    _pid = getpid();
}

void ring_log::init_path(const char* log_dir, const char* prog_name, int level)
{
    pthread_mutex_lock(&_mutex);

    // 日志记录目录
    strncpy(_log_dir, log_dir, 512);
    //name format:  name_year-mon-day-t[tid].log.n
    strncpy(_prog_name, prog_name, 128);

    mkdir(_log_dir, 0777);
    //查看是否存在此目录、目录下是否允许创建文件
    /*
     * 权限检查
     */
    if (access(_log_dir, F_OK | W_OK) == -1)
    {
        fprintf(stderr, "logdir: %s error: %s\n", _log_dir, strerror(errno));
    }
    else
    {
    	// 环境检查通过
        _env_ok = true;
    }
    if (level > TRACE)
        level = TRACE;
    if (level < FATAL)
        level = FATAL;
    _level = level;

    pthread_mutex_unlock(&_mutex);
}

// 消费者
void ring_log::persist()
{
    while (true)
    {
        //check if _prst_buf need to be persist
        pthread_mutex_lock(&_mutex);
        if (_prst_buf->status == cell_buffer::FREE)
        {
            struct timespec tsp;
            struct timeval now;
            gettimeofday(&now, NULL);
            tsp.tv_sec = now.tv_sec;
            tsp.tv_nsec = now.tv_usec * 1000;//nanoseconds
            tsp.tv_sec += BUFF_WAIT_TIME;//wait for 1 seconds
            /*
             * 等待1s
             */
            pthread_cond_timedwait(&_cond, &_mutex, &tsp);
        }
        if (_prst_buf->empty())
        {
        	/*
        	 * 1s后仍然没有日志，重新迭代
        	 */
            //give up, go to next turn
            pthread_mutex_unlock(&_mutex);
            continue;
        }

        if (_prst_buf->status == cell_buffer::FREE)
        {
        	// 有日志，但未填满：生产者应该也指向这里
            assert(_curr_buf == _prst_buf);//to test
            // 将生产者指向后面的一个ring buffer，后面持久化该ring buffer
            _curr_buf->status = cell_buffer::FULL;
            _curr_buf = _curr_buf->next;
        }

        int year = _tm.year, mon = _tm.mon, day = _tm.day;
        pthread_mutex_unlock(&_mutex);

        //decision which file to write
        if (!decis_file(year, mon, day))
            continue;
        //write
        _prst_buf->persist(_fp);
        // 刷缓冲，不如直接direct+异步
        fflush(_fp);

        pthread_mutex_lock(&_mutex);
        // 清空当前ring buffer，并移动到下一个ring buffer
        _prst_buf->clear();
        _prst_buf = _prst_buf->next;
        pthread_mutex_unlock(&_mutex);
    }
}

void ring_log::try_append(const char* lvl, const char* format, ...)
{
    int ms;
    // 拿到ms同时更新s
    uint64_t curr_sec = _tm.get_curr_time(&ms);
    // 5s
    if (_lst_lts && curr_sec - _lst_lts < RELOG_THRESOLD)
        return ;

    // 4KB
    char log_line[LOG_LEN_LIMIT];
    // 1）日志头部时间
    //int prev_len = snprintf(log_line, LOG_LEN_LIMIT, "%s[%d-%02d-%02d %02d:%02d:%02d.%03d]", lvl, _tm.year, _tm.mon, _tm.day, _tm.hour, _tm.min, _tm.sec, ms);
    int prev_len = snprintf(log_line, LOG_LEN_LIMIT, "%s[%s.%03d]", lvl, _tm.utc_fmt, ms);

    va_list arg_ptr;
    va_start(arg_ptr, format);

    // 2）日志内容
    //TO OPTIMIZE IN THE FUTURE: performance too low here!
    int main_len = vsnprintf(log_line + prev_len, LOG_LEN_LIMIT - prev_len, format, arg_ptr);

    va_end(arg_ptr);

    uint32_t len = prev_len + main_len;

    // 重置最近更新
    _lst_lts = 0;
    bool tell_back = false;

    pthread_mutex_lock(&_mutex);
    if (_curr_buf->status == cell_buffer::FREE && _curr_buf->avail_len() >= len)
    {
    	// 消耗当前ring buffer
        _curr_buf->append(log_line, len);
    }
    else
    {
        //1. _curr_buf->status = cell_buffer::FREE but _curr_buf->avail_len() < len
        //2. _curr_buf->status = cell_buffer::FULL
        if (_curr_buf->status == cell_buffer::FREE)
        {
        	// 当前rong buffer还空余缓冲但不足本地填充，直接填充下一个
            _curr_buf->status = cell_buffer::FULL;//set to FULL
            cell_buffer* next_buf = _curr_buf->next;
            //tell backend thread
            // 移动了ring buffer需要唤醒持久化线程
            tell_back = true;

            //it suggest that this buffer is under the persist job
            // 所有ring buffer完全填满
            if (next_buf->status == cell_buffer::FULL)
            {
            	// ring buffer不超过3GB
                //if mem use < MEM_USE_LIMIT, allocate new cell_buffer
                if (_one_buff_len * (_buff_cnt + 1) > MEM_USE_LIMIT)
                {
                    fprintf(stderr, "no more log space can use\n");
                    _curr_buf = next_buf;
                    _lst_lts = curr_sec;
                }
                else
                {
                	// 新分配一个ring buffer，挂链
                    cell_buffer* new_buffer = new cell_buffer(_one_buff_len);
                    _buff_cnt += 1;
                    new_buffer->prev = _curr_buf;
                    _curr_buf->next = new_buffer;
                    new_buffer->next = next_buf;
                    next_buf->prev = new_buffer;
                    _curr_buf = new_buffer;
                }
            }
            else
            {
                //next buffer is free, we can use it
                _curr_buf = next_buf;
            }
            if (!_lst_lts)
                _curr_buf->append(log_line, len);
        }
        else//_curr_buf->status == cell_buffer::FULL, assert persist is on here too!
        {
            _lst_lts = curr_sec;
        }
    }
    pthread_mutex_unlock(&_mutex);
    // 唤醒持久化线程
    if (tell_back)
    {
        pthread_cond_signal(&_cond);
    }
}

bool ring_log::decis_file(int year, int mon, int day)
{
    //TODO: 是根据日志消息的时间写时间？还是自主写时间？  I select 自主写时间
    if (!_env_ok)
    {
        if (_fp)
            fclose(_fp);
        _fp = fopen("/dev/null", "w");
        return _fp != NULL;
    }
    if (!_fp)
    {
        _year = year, _mon = mon, _day = day;
        // 写死的1024
        char log_path[1024] = {};
        sprintf(log_path, "%s/%s.%d%02d%02d.%u.log", _log_dir, _prog_name, _year, _mon, _day, _pid);
        _fp = fopen(log_path, "w");
        if (_fp)
            _log_cnt += 1;
    }
    else if (_day != day)
    {
        fclose(_fp);
        char log_path[1024] = {};
        _year = year, _mon = mon, _day = day;
        sprintf(log_path, "%s/%s.%d%02d%02d.%u.log", _log_dir, _prog_name, _year, _mon, _day, _pid);
        _fp = fopen(log_path, "w");
        if (_fp)
            _log_cnt = 1;
    }
    else if (ftell(_fp) >= LOG_USE_LIMIT)
    {
        fclose(_fp);
        char old_path[1024] = {};
        char new_path[1024] = {};
        //mv xxx.log.[i] xxx.log.[i + 1]
        for (int i = _log_cnt - 1;i > 0; --i)
        {
            sprintf(old_path, "%s/%s.%d%02d%02d.%u.log.%d", _log_dir, _prog_name, _year, _mon, _day, _pid, i);
            sprintf(new_path, "%s/%s.%d%02d%02d.%u.log.%d", _log_dir, _prog_name, _year, _mon, _day, _pid, i + 1);
            rename(old_path, new_path);
        }
        //mv xxx.log xxx.log.1
        sprintf(old_path, "%s/%s.%d%02d%02d.%u.log", _log_dir, _prog_name, _year, _mon, _day, _pid);
        sprintf(new_path, "%s/%s.%d%02d%02d.%u.log.1", _log_dir, _prog_name, _year, _mon, _day, _pid);
        rename(old_path, new_path);
        _fp = fopen(old_path, "w");
        if (_fp)
            _log_cnt += 1;
    }
    return _fp != NULL;
}

void* be_thdo(void* args)
{
    ring_log::ins()->persist();
    return NULL;
}
