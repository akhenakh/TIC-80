#include "control.h"
#include "api.h"
#include "tic.h"
#include "ext/png.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET socket_t;
  #define CLOSE_SOCKET(s) closesocket(s)
  #define IS_VALID_SOCKET(s) ((s) != INVALID_SOCKET)
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <errno.h>
  typedef int socket_t;
  #define INVALID_SOCKET (-1)
  #define SOCKET_ERROR (-1)
  #define CLOSE_SOCKET(s) close(s)
  #define IS_VALID_SOCKET(s) ((s) >= 0)
#endif

#define MAX_CLIENTS 4
#define RECV_BUFFER_SIZE 2048

typedef struct
{
    socket_t sock;
    char buffer[RECV_BUFFER_SIZE];
    size_t buf_len;
} ClientConnection;

struct tic_control
{
    socket_t server_sock;
    ClientConnection clients[MAX_CLIENTS];

    tic80_gamepads gamepads;
    tic80_mouse mouse;
    bool mouse_active;
    tic_key active_keys[TIC80_KEY_BUFFER];
};

static void set_nonblocking(socket_t fd)
{
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags != -1)
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

tic_control* tic_control_create(int port)
{
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
#endif

    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (!IS_VALID_SOCKET(s)) return NULL;

    int opt = 1;
#ifdef _WIN32
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    set_nonblocking(s);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(s, (struct sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        CLOSE_SOCKET(s);
        return NULL;
    }

    if (listen(s, MAX_CLIENTS) == SOCKET_ERROR)
    {
        CLOSE_SOCKET(s);
        return NULL;
    }

    tic_control* ctrl = (tic_control*)calloc(1, sizeof(tic_control));
    ctrl->server_sock = s;
    for (int i = 0; i < MAX_CLIENTS; i++)
        ctrl->clients[i].sock = INVALID_SOCKET;

    printf("[Control Socket] Listening on port %d\n", port);
    return ctrl;
}

static void send_all(socket_t sock, const void* data, size_t len)
{
    const char* ptr = (const char*)data;
    while (len > 0)
    {
        int sent = send(sock, ptr, (int)len, 0);
        if (sent <= 0) break;
        ptr += sent;
        len -= sent;
    }
}

static void send_reply(socket_t sock, const char* msg)
{
    send_all(sock, msg, strlen(msg));
}

static int parse_button_id(const char* name)
{
    if (!name) return -1;
    if (isdigit((unsigned char)name[0])) return atoi(name);

    if (strcasecmp(name, "UP") == 0)    return 0;
    if (strcasecmp(name, "DOWN") == 0)  return 1;
    if (strcasecmp(name, "LEFT") == 0)  return 2;
    if (strcasecmp(name, "RIGHT") == 0) return 3;
    if (strcasecmp(name, "A") == 0)     return 4;
    if (strcasecmp(name, "B") == 0)     return 5;
    if (strcasecmp(name, "X") == 0)     return 6;
    if (strcasecmp(name, "Y") == 0)     return 7;

    return -1;
}

static tic_key parse_key_code(const char* name)
{
    if (!name || !name[0]) return tic_key_unknown;
    if (name[1] == '\0')
    {
        char c = tolower((unsigned char)name[0]);
        if (c >= 'a' && c <= 'z') return (tic_key)(tic_key_a + (c - 'a'));
        if (c >= '0' && c <= '9') return (tic_key)(tic_key_0 + (c - '0'));
    }

    if (strcasecmp(name, "SPACE") == 0)     return tic_key_space;
    if (strcasecmp(name, "RETURN") == 0 ||
        strcasecmp(name, "ENTER") == 0)     return tic_key_return;
    if (strcasecmp(name, "TAB") == 0)       return tic_key_tab;
    if (strcasecmp(name, "BACKSPACE") == 0) return tic_key_backspace;
    if (strcasecmp(name, "DELETE") == 0)    return tic_key_delete;
    if (strcasecmp(name, "INSERT") == 0)    return tic_key_insert;
    if (strcasecmp(name, "UP") == 0)        return tic_key_up;
    if (strcasecmp(name, "DOWN") == 0)      return tic_key_down;
    if (strcasecmp(name, "LEFT") == 0)      return tic_key_left;
    if (strcasecmp(name, "RIGHT") == 0)     return tic_key_right;
    if (strcasecmp(name, "SHIFT") == 0)     return tic_key_shift;
    if (strcasecmp(name, "CTRL") == 0)      return tic_key_ctrl;
    if (strcasecmp(name, "ALT") == 0)       return tic_key_alt;
    if (strcasecmp(name, "ESCAPE") == 0 ||
        strcasecmp(name, "ESC") == 0)       return tic_key_escape;

    int num = atoi(name);
    if (num > 0 && num < tic_keys_count) return (tic_key)num;

    return tic_key_unknown;
}

static bool crop_screen_region(int* x, int* y, int* w, int* h)
{
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x >= TIC80_WIDTH || *y >= TIC80_HEIGHT) return false;
    if (*x + *w > TIC80_WIDTH)  *w = TIC80_WIDTH - *x;
    if (*y + *h > TIC80_HEIGHT) *h = TIC80_HEIGHT - *y;
    return (*w > 0 && *h > 0);
}

static u32* extract_region_pixels(tic80* tic, int x, int y, int w, int h)
{
    u32* region = (u32*)malloc(w * h * sizeof(u32));
    if (!region) return NULL;

    for (int row = 0; row < h; row++)
    {
        int src_y = y + row + TIC80_OFFSET_TOP;
        int src_x = x + TIC80_OFFSET_LEFT;
        memcpy(&region[row * w], &tic->screen[src_y * TIC80_FULLWIDTH + src_x], w * sizeof(u32));
    }
    return region;
}

static void handle_screenshot(tic_control* ctrl, socket_t sock, tic80* tic, char* args, const char* format)
{
    int rx = 0, ry = 0, rw = TIC80_WIDTH, rh = TIC80_HEIGHT;
    if (args && strlen(args) > 0)
    {
        if (sscanf(args, "%d %d %d %d", &rx, &ry, &rw, &rh) != 4)
        {
            send_reply(sock, "ERR usage: SCREENSHOT [x y w h]\n");
            return;
        }
    }

    if (!crop_screen_region(&rx, &ry, &rw, &rh))
    {
        send_reply(sock, "ERR invalid region bounds\n");
        return;
    }

    u32* pixels = extract_region_pixels(tic, rx, ry, rw, rh);
    if (!pixels)
    {
        send_reply(sock, "ERR out of memory\n");
        return;
    }

    if (strcmp(format, "PPM") == 0)
    {
        char header[64];
        int hlen = snprintf(header, sizeof(header), "P6\n%d %d\n255\n", rw, rh);
        send_all(sock, header, hlen);

        u8* rgb_buf = (u8*)malloc(rw * rh * 3);
        if (rgb_buf)
        {
            for (int i = 0; i < rw * rh; i++)
            {
                u32 val = pixels[i];
                rgb_buf[i * 3 + 0] = (val >> 0) & 0xFF; // R
                rgb_buf[i * 3 + 1] = (val >> 8) & 0xFF; // G
                rgb_buf[i * 3 + 2] = (val >> 16) & 0xFF;// B
            }
            send_all(sock, rgb_buf, rw * rh * 3);
            free(rgb_buf);
        }
    }
    else if (strcmp(format, "RAW") == 0)
    {
        char header[64];
        int size = rw * rh * (int)sizeof(u32);
        int hlen = snprintf(header, sizeof(header), "OK RAW %d %d %d\n", rw, rh, size);
        send_all(sock, header, hlen);
        send_all(sock, pixels, size);
    }
    else // Default: PNG
    {
        png_img img = {
            .width = rw,
            .height = rh,
            .values = pixels
        };
        png_buffer png = png_write(img, (png_buffer){0});
        if (png.data && png.size > 0)
        {
            char header[64];
            int hlen = snprintf(header, sizeof(header), "OK PNG %d\n", png.size);
            send_all(sock, header, hlen);
            send_all(sock, png.data, png.size);
            free(png.data);
        }
        else
        {
            send_reply(sock, "ERR PNG encoding failed\n");
        }
    }

    free(pixels);
}

static void handle_command(tic_control* ctrl, socket_t sock, tic80* tic, char* line, bool* request_quit)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return;

    char cmd[32];
    int n = 0;
    while (*line && !isspace((unsigned char)*line) && n < (int)sizeof(cmd) - 1)
        cmd[n++] = toupper((unsigned char)*line++);
    cmd[n] = '\0';
    while (*line == ' ' || *line == '\t') line++;

    if (strcmp(cmd, "PING") == 0)
    {
        send_reply(sock, "PONG\n");
    }
    else if (strcmp(cmd, "SCREENSHOT") == 0 || strcmp(cmd, "SCREENSHOT_PNG") == 0)
    {
        handle_screenshot(ctrl, sock, tic, line, "PNG");
    }
    else if (strcmp(cmd, "SCREENSHOT_PPM") == 0)
    {
        handle_screenshot(ctrl, sock, tic, line, "PPM");
    }
    else if (strcmp(cmd, "SCREENSHOT_RAW") == 0)
    {
        handle_screenshot(ctrl, sock, tic, line, "RAW");
    }
    else if (strcmp(cmd, "BTN") == 0)
    {
        char bname[16];
        int player = 0, state = 0;
        int parsed = sscanf(line, "%15s %d %d", bname, &state, &player);
        if (parsed < 2)
        {
            send_reply(sock, "ERR usage: BTN <name|id> <0|1> [player:0-3]\n");
            return;
        }
        int btn = parse_button_id(bname);
        if (btn < 0 || btn > 7 || player < 0 || player > 3)
        {
            send_reply(sock, "ERR invalid button or player index\n");
            return;
        }

        u8* pad = &((u8*)&ctrl->gamepads)[player];
        if (state) *pad |= (1 << btn);
        else       *pad &= ~(1 << btn);

        send_reply(sock, "OK\n");
    }
    else if (strcmp(cmd, "BTNS") == 0)
    {
        unsigned int mask = 0;
        int player = 0;
        if (sscanf(line, "%i %d", &mask, &player) < 1 || player < 0 || player > 3)
        {
            send_reply(sock, "ERR usage: BTNS <bitmask> [player:0-3]\n");
            return;
        }
        ((u8*)&ctrl->gamepads)[player] = (u8)(mask & 0xFF);
        send_reply(sock, "OK\n");
    }
    else if (strcmp(cmd, "MOUSE") == 0)
    {
        int x = 0, y = 0, left = 0, mid = 0, right = 0, sx = 0, sy = 0;
        int count = sscanf(line, "%d %d %d %d %d %d %d", &x, &y, &left, &mid, &right, &sx, &sy);
        if (count < 2)
        {
            send_reply(sock, "ERR usage: MOUSE <x> <y> [left] [middle] [right] [scrollx] [scrolly]\n");
            return;
        }
        ctrl->mouse.x = (s16)(x + TIC80_OFFSET_LEFT);
        ctrl->mouse.y = (s16)(y + TIC80_OFFSET_TOP);
        if (count >= 3) ctrl->mouse.left = left ? 1 : 0;
        if (count >= 4) ctrl->mouse.middle = mid ? 1 : 0;
        if (count >= 5) ctrl->mouse.right = right ? 1 : 0;
        if (count >= 6) ctrl->mouse.scrollx = (s8)sx;
        if (count >= 7) ctrl->mouse.scrolly = (s8)sy;
        ctrl->mouse_active = true;

        send_reply(sock, "OK\n");
    }
    else if (strcmp(cmd, "KEY") == 0)
    {
        char kname[32];
        int state = 0;
        if (sscanf(line, "%31s %d", kname, &state) != 2)
        {
            send_reply(sock, "ERR usage: KEY <key_name|code> <0|1>\n");
            return;
        }
        tic_key k = parse_key_code(kname);
        if (k == tic_key_unknown)
        {
            send_reply(sock, "ERR unknown keycode\n");
            return;
        }

        if (state)
        {
            bool inserted = false;
            for (int i = 0; i < TIC80_KEY_BUFFER; i++)
            {
                if (ctrl->active_keys[i] == k) { inserted = true; break; }
            }
            if (!inserted)
            {
                for (int i = 0; i < TIC80_KEY_BUFFER; i++)
                {
                    if (ctrl->active_keys[i] == tic_key_unknown)
                    {
                        ctrl->active_keys[i] = k;
                        break;
                    }
                }
            }
        }
        else
        {
            for (int i = 0; i < TIC80_KEY_BUFFER; i++)
            {
                if (ctrl->active_keys[i] == k)
                    ctrl->active_keys[i] = tic_key_unknown;
            }
        }
        send_reply(sock, "OK\n");
    }
    else if (strcmp(cmd, "RESET_INPUTS") == 0)
    {
        memset(&ctrl->gamepads, 0, sizeof(ctrl->gamepads));
        memset(&ctrl->mouse, 0, sizeof(ctrl->mouse));
        ctrl->mouse_active = false;
        memset(ctrl->active_keys, 0, sizeof(ctrl->active_keys));
        send_reply(sock, "OK\n");
    }
    else if (strcmp(cmd, "RESET") == 0)
    {
        tic_api_reset((tic_mem*)tic);
        send_reply(sock, "OK\n");
    }
    else if (strcmp(cmd, "EXIT") == 0 || strcmp(cmd, "QUIT") == 0)
    {
        if (request_quit) *request_quit = true;
        send_reply(sock, "OK\n");
    }
    else
    {
        send_reply(sock, "ERR unknown command\n");
    }
}

void tic_control_poll(tic_control* ctrl, tic80* tic, tic80_input* input, bool* request_quit)
{
    if (!ctrl) return;

    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    socket_t incoming = accept(ctrl->server_sock, (struct sockaddr*)&client_addr, &addr_len);
    if (IS_VALID_SOCKET(incoming))
    {
        set_nonblocking(incoming);
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
        {
            if (!IS_VALID_SOCKET(ctrl->clients[i].sock))
            {
                slot = i;
                break;
            }
        }
        if (slot >= 0)
        {
            ctrl->clients[slot].sock = incoming;
            ctrl->clients[slot].buf_len = 0;
        }
        else
        {
            CLOSE_SOCKET(incoming);
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        ClientConnection* c = &ctrl->clients[i];
        if (!IS_VALID_SOCKET(c->sock)) continue;

        int available = (int)(RECV_BUFFER_SIZE - 1 - c->buf_len);
        if (available > 0)
        {
            int r = recv(c->sock, c->buffer + c->buf_len, available, 0);
            if (r > 0)
            {
                c->buf_len += r;
                c->buffer[c->buf_len] = '\0';
            }
            else if (r == 0 || (r < 0 &&
#ifdef _WIN32
                WSAGetLastError() != WSAEWOULDBLOCK
#else
                errno != EAGAIN && errno != EWOULDBLOCK
#endif
            ))
            {
                CLOSE_SOCKET(c->sock);
                c->sock = INVALID_SOCKET;
                c->buf_len = 0;
                continue;
            }
        }

        char* nl;
        while ((nl = strchr(c->buffer, '\n')) != NULL)
        {
            *nl = '\0';
            if (nl > c->buffer && *(nl - 1) == '\r')
                *(nl - 1) = '\0';

            handle_command(ctrl, c->sock, tic, c->buffer, request_quit);

            size_t processed = (nl - c->buffer) + 1;
            memmove(c->buffer, nl + 1, c->buf_len - processed);
            c->buf_len -= processed;
            c->buffer[c->buf_len] = '\0';
        }
    }

    if (input)
    {
        input->gamepads.data |= ctrl->gamepads.data;

        if (ctrl->mouse_active)
        {
            input->mouse.x = ctrl->mouse.x;
            input->mouse.y = ctrl->mouse.y;
            input->mouse.left |= ctrl->mouse.left;
            input->mouse.middle |= ctrl->mouse.middle;
            input->mouse.right |= ctrl->mouse.right;
            input->mouse.scrollx = ctrl->mouse.scrollx;
            input->mouse.scrolly = ctrl->mouse.scrolly;
            ctrl->mouse.scrollx = 0;
            ctrl->mouse.scrolly = 0;
        }

        for (int k = 0; k < TIC80_KEY_BUFFER; k++)
        {
            tic_key key = ctrl->active_keys[k];
            if (key != tic_key_unknown)
            {
                for (int slot = 0; slot < TIC80_KEY_BUFFER; slot++)
                {
                    if (input->keyboard.keys[slot] == key) break;
                    if (input->keyboard.keys[slot] == tic_key_unknown)
                    {
                        input->keyboard.keys[slot] = key;
                        break;
                    }
                }
            }
        }
    }
}

void tic_control_close(tic_control* ctrl)
{
    if (!ctrl) return;
    for (int i = 0; i < MAX_CLIENTS; i++)
    {
        if (IS_VALID_SOCKET(ctrl->clients[i].sock))
            CLOSE_SOCKET(ctrl->clients[i].sock);
    }
    if (IS_VALID_SOCKET(ctrl->server_sock))
        CLOSE_SOCKET(ctrl->server_sock);

#ifdef _WIN32
    WSACleanup();
#endif

    free(ctrl);
}
