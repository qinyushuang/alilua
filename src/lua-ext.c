#include <sys/stat.h>

#include "config.h"
#include "main.h"
#include "network.h"
#include "lua-ext.h"

static char temp_buf[8192];

#define TIME_BUCKET_SIZE 640
static sleep_timeout_t *timeout_links[TIME_BUCKET_SIZE] = {0};
static sleep_timeout_t *timeout_link_ends[TIME_BUCKET_SIZE] = {0};
static long now_4sleep;

int check_lua_sleep_timeouts()
{
    now_4sleep = longtime() / 10;
    int _k = now_4sleep % TIME_BUCKET_SIZE;
    int k = _k - 1;

    for(; k < _k + 1; k++) {
        sleep_timeout_t *m = timeout_links[k], *n = NULL;
        lua_State *L = NULL;

        while(m) {
            n = m;
            m = m->next;

            if(now_4sleep >= n->timeout) { // timeout
                {
                    if(n->uper) {
                        ((sleep_timeout_t *) n->uper)->next = n->next;

                    } else {
                        timeout_links[k] = n->next;
                    }

                    if(n->next) {
                        ((sleep_timeout_t *) n->next)->uper = n->uper;

                    } else {
                        timeout_link_ends[k] = n->uper;
                    }

                    L = n->L;
                    free(n);
                }

                if(L) {
                    lua_resume(L, 0);
                }

                L = NULL;
            }
        }
    }

    return 1;
}

int _lua_sleep(lua_State *L, int sec)
{
    now_4sleep = longtime() / 10;
    sleep_timeout_t *n = malloc(sizeof(sleep_timeout_t));

    if(!n) {
        return 0;
    }

    sec /= 10;

    if(sec < 1) {
        sec = 1;
    }

    n->timeout = now_4sleep + sec;
    n->uper = NULL;
    n->next = NULL;
    n->L = L;

    int k = n->timeout % TIME_BUCKET_SIZE;

    if(timeout_link_ends[k] == NULL) {
        timeout_links[k] = n;
        timeout_link_ends[k] = n;

    } else { // add to link end
        timeout_link_ends[k]->next = n;
        n->uper = timeout_link_ends[k];
        timeout_link_ends[k] = n;
    }

    return lua_yield(L, 0);
}

int lua_f_sleep(lua_State *L)
{
    if(!lua_isnumber(L, 1)) {
        return 0;
    }

    int sec = lua_tonumber(L, 1);

    if(sec < 1) {
        return 0;
    }

    if(sec > 1000000) {
        return lua_yield(L, 0);       // for ever
    }

    return _lua_sleep(L, sec);
}

static epdata_t *get_epd(lua_State *L)
{
    epdata_t *epd = NULL;

    lua_getglobal(L, "__epd__");

    if(lua_isuserdata(L, -1)) {
        epd = lua_touserdata(L, -1);
    }

    lua_pop(L, 1);

    return epd;
}

int lua_check_timeout(lua_State *L)
{
    epdata_t *epd = get_epd(L);

    if(!epd) {
        lua_pushnil(L);
        lua_pushstring(L, "miss epd!");
        return 2;
    }

    if(epd->websocket) {
        return 0;
    }

    if(longtime() - epd->start_time > STEP_PROCESS_TIMEOUT) {
        epd->keepalive = 0;
        //network_be_end(epd);
        lua_pushstring(L, "Process Time Out!");
        lua_error(L);    /// stop lua script
    }

    return 0;
}

int lua_header(lua_State *L)
{
    epdata_t *epd = get_epd(L);

    if(!epd) {
        lua_pushnil(L);
        lua_pushstring(L, "miss epd!");
        return 2;
    }

    if(epd->websocket) {
        return 0;
    }

    if(lua_gettop(L) < 1) {
        return 0;
    }

    int t = lua_type(L, 1);
    size_t dlen = 0;
    const char *data = NULL;

    if(t == LUA_TSTRING) {
        data = lua_tolstring(L, 1, &dlen);

        if(stristr(data, "content-length", dlen) != data) {
            network_send_header(epd, data);
        }

    } else if(t == LUA_TTABLE) {
        int len = lua_objlen(L, 1), i = 0;

        for(i = 0; i < len; i++) {
            lua_pushinteger(L, i + 1);
            lua_gettable(L, -2);

            if(lua_isstring(L, -1)) {
                data = lua_tolstring(L, -1, &dlen);

                if(stristr(data, "content-length", dlen) != data) {
                    network_send_header(epd, lua_tostring(L, -1));
                }
            }

            lua_pop(L, 1);
        }
    }

    return 0;
}
static void _lua_echo(epdata_t *epd, lua_State *L, int nargs)
{
    size_t len = 0;

    if(lua_istable(L, 1)) {
        len = lua_calc_strlen_in_table(L, 1, 2, 0 /* strict */);

        if(len < 1) {
            return;
        }

        char *buf = temp_buf;

        if(len > 8192) {
            buf = malloc(len);

            if(!buf) {
                return;
            }

            lua_copy_str_in_table(L, 1, buf);
            network_send(epd, buf, len);
            free(buf);

        } else {
            lua_copy_str_in_table(L, 1, buf);
            network_send(epd, buf, len);
        }

    } else {
        const char *data = NULL;
        int i = 0;

        for(i = 1; i <= nargs; i++) {
            if(lua_isboolean(L, i)) {
                if(lua_toboolean(L, i)) {
                    network_send(epd, "true", 4);

                } else {
                    network_send(epd, "false", 5);
                }

            } else {
                data = lua_tolstring(L, i, &len);
                network_send(epd, data, len);
            }
        }
    }
}

int lua_echo(lua_State *L)
{
    epdata_t *epd = get_epd(L);

    if(!epd) {
        lua_pushnil(L);
        lua_pushstring(L, "miss epd!");
        return 2;
    }

    int nargs = lua_gettop(L);

    if(nargs < 1) {
        luaL_error(L, "miss content!");
        return 0;
    }

    size_t len = 0;

    if(epd->websocket) {
        return 0;
    }

    _lua_echo(epd, L, nargs);

    return 0;
}

int lua_clear_header(lua_State *L)
{
    epdata_t *epd = get_epd(L);

    if(!epd) {
        lua_pushnil(L);
        lua_pushstring(L, "miss epd!");
        return 2;
    }

    epd->response_header_length = 0;
    free(epd->iov[0].iov_base);
    epd->iov[0].iov_base = NULL;
    epd->iov[0].iov_len = 0;
    return 0;
}

#ifdef __APPLE__
#ifndef st_mtime
#define st_mtime st_mtimespec.tv_sec
#endif
#endif
static const char *DAYS_OF_WEEK[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *MONTHS_OF_YEAR[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static char _gmt_time[64] = {0};
int network_sendfile(epdata_t *epd, const char *path)
{
    if(epd->process_timeout == 1) {
        return 0;
    }

    struct stat st;

    if((epd->response_sendfile_fd = open(path, O_RDONLY)) < 0) {
        epd->response_sendfile_fd = -2;
        //printf ( "Can't open '%s' file\n", path );
        return 0;
    }

    if(fstat(epd->response_sendfile_fd, &st) == -1) {
        close(epd->response_sendfile_fd);
        epd->response_sendfile_fd = -2;
        //printf ( "Can't stat '%s' file\n", path );
        return 0;
    }

    epd->response_content_length = st.st_size;
    epd->response_buf_sended = 0;

    /// clear send bufs;!!!
    int i = 0;

    for(i = 1; i < epd->iov_buf_count; i++) {
        free(epd->iov[i].iov_base);
        epd->iov[i].iov_base = NULL;
        epd->iov[i].iov_len = 0;
    }

    epd->iov_buf_count = 0;

    struct tm *_clock;
    _clock = gmtime(&(st.st_mtime));
    sprintf(_gmt_time, "%s, %02d %s %04d %02d:%02d:%02d GMT",
            DAYS_OF_WEEK[_clock->tm_wday],
            _clock->tm_mday,
            MONTHS_OF_YEAR[_clock->tm_mon],
            _clock->tm_year + 1900,
            _clock->tm_hour,
            _clock->tm_min,
            _clock->tm_sec);

    if(epd->if_modified_since && strcmp(_gmt_time, epd->if_modified_since) == 0) {
        epd->response_header_length = 0;
        free(epd->iov[0].iov_base);
        epd->iov[0].iov_base = NULL;
        epd->iov[0].iov_len = 0;
        network_send_header(epd, "HTTP/1.1 304 Not Modified");
        close(epd->response_sendfile_fd);
        epd->response_sendfile_fd = -1;
        epd->response_content_length = 0;
        return 1;
    }

    sprintf(temp_buf, "Content-Type: %s", get_mime_type(path));
    network_send_header(epd, temp_buf);

    sprintf(temp_buf, "Last-Modified: %s", _gmt_time);
    network_send_header(epd, temp_buf);

    if(temp_buf[14] == 't' && temp_buf[15] == 'e') {
        int fd = epd->response_sendfile_fd;
        epd->response_sendfile_fd = -1;
        epd->response_content_length = 0;
        int n = 0;

        while((n = read(fd, &temp_buf, 4096)) > 0) {
            network_send(epd, temp_buf, n);
        }

        if(n < 0) {
        }

        close(fd);

        return 1;
    }

#ifdef linux
    int set = 1;
    setsockopt(epd->fd, IPPROTO_TCP, TCP_CORK, &set, sizeof(int));
#endif
    return 1;
}

int lua_sendfile(lua_State *L)
{
    epdata_t *epd = get_epd(L);

    if(!epd) {
        lua_pushnil(L);
        lua_pushstring(L, "miss epd!");
        return 2;
    }

    if(!lua_isstring(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "Need a file path!");
        return 2;
    }

    size_t len = 0;
    const char *fname = lua_tolstring(L, 1, &len);
    //char *full_fname = malloc(epd->vhost_root_len + len);
    char *full_fname = (char *)&temp_buf;
    memcpy(full_fname, epd->vhost_root, epd->vhost_root_len);
    memcpy(full_fname + epd->vhost_root_len , fname, len);
    full_fname[epd->vhost_root_len + len] = '\0';

    network_sendfile(epd, full_fname);

    //free(full_fname);

    network_be_end(epd);
    lua_pushnil(L);
    lua_error(L); /// stop lua script
    return 0;
}

int lua_end(lua_State *L)
{
    epdata_t *epd = get_epd(L);

    if(!epd) {
        return 0;
    }

    if(epd->status != STEP_PROCESS) {
        return 0;
    }

    if(epd->websocket || epd->status == STEP_SEND) {
        return 0;
    }

    network_be_end(epd);
    return 0;
}

int lua_die(lua_State *L)
{
    epdata_t *epd = get_epd(L);

    if(!epd) {
        return 0;
    }

    int nargs = lua_gettop(L);
    _lua_echo(epd, L, nargs);

    if(epd->status != STEP_PROCESS) {
        return 0;
    }

    if(epd->websocket || epd->status == STEP_SEND) {
        return 0;
    }

    lua_pushnil(L);
    lua_error(L); /// stop lua script

    //network_be_end(epd);

    return 0;
}

int lua_get_post_body(lua_State *L)
{
    epdata_t *epd = get_epd(L);

    if(!epd) {
        lua_pushnil(L);
        lua_pushstring(L, "miss epd!");
        return 2;
    }

    if(epd->websocket) {
        return 0;
    }

    if(epd->content_length > 0) {
        lua_pushlstring(L, epd->contents, epd->content_length);

    } else {
        lua_pushnil(L);
    }

    epd->contents = NULL;
    epd->content_length = 0;

    return 1;
}

int lua_f_random_string(lua_State *L)
{
    int size = 32;

    if(lua_gettop(L) == 1 && lua_isnumber(L, 1)) {
        size = lua_tonumber(L, 1);
    }

    if(size < 1) {
        size = 32;
    }

    if(size > 4096) {
        return 0;
    }

    random_string(temp_buf, size, 0);

    lua_pushlstring(L, temp_buf, size);

    return 1;
}

int lua_f_file_exists(lua_State *L)
{
    epdata_t *epd = get_epd(L);

    if(!epd) {
        lua_pushnil(L);
        lua_pushstring(L, "miss epd!");
        return 2;
    }

    if(!lua_isstring(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "Need a file path!");
        return 2;
    }

    size_t len = 0;
    const char *fname = lua_tolstring(L, 1, &len);
    //char *full_fname = malloc(epd->vhost_root_len + len);
    char *full_fname = (char *)&temp_buf;
    memcpy(full_fname, epd->vhost_root, epd->vhost_root_len);
    memcpy(full_fname + epd->vhost_root_len, fname, len);
    full_fname[epd->vhost_root_len + len] = '\0';

    lua_pushboolean(L, access(full_fname, F_OK) != -1);

    //free(full_fname);

    return 1;
}

int lua_f_readfile(lua_State *L)
{
    epdata_t *epd = get_epd(L);

    if(!epd) {
        lua_pushnil(L);
        lua_pushstring(L, "miss epd!");
        return 2;
    }

    if(!lua_isstring(L, -1)) {
        lua_pushnil(L);
        lua_pushstring(L, "Need a file path!");
        return 2;
    }

    size_t len = 0;
    const char *fname = lua_tolstring(L, 1, &len);
    //char *full_fname = malloc(epd->vhost_root_len + len);
    char *full_fname = (char *)&temp_buf;
    memcpy(full_fname, epd->vhost_root, epd->vhost_root_len);
    memcpy(full_fname + epd->vhost_root_len , fname, len);
    full_fname[epd->vhost_root_len + len] = '\0';

    char *buf = NULL;
    off_t reads = 0;
    int fd = open(full_fname, O_RDONLY, 0);

    //printf("readfile: %s\n", full_fname);
    //free(full_fname);

    if(fd > -1) {
        reads = lseek(fd, 0L, SEEK_END);
        lseek(fd, 0L, SEEK_SET);

        if(reads > 8192) {
            buf = malloc(reads);

        } else {
            buf = (char *)&temp_buf;
        }

        read(fd, buf, reads);

        close(fd);

        lua_pushlstring(L, buf, reads);

        if(buf != (char *)&temp_buf) {
            free(buf);
        }

        return 1;
    }

    lua_pushnil(L);
    lua_pushstring(L, strerror(errno));

    return 2;
}

