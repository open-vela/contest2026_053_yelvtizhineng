/* Small bounded HTTP response reader shared by JSON GET and POST.
 * Complete framed bodies immediately, even when the peer keeps TCP open.
 * A timeout, truncated body or capacity overflow is never a valid response.
 */
#ifndef PLANT_HTTP_RESPONSE_H
#define PLANT_HTTP_RESPONSE_H
#include <sys/socket.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include "http_chunked.h"

struct plant_http_body
{
  char *out;
  size_t capacity, used;
};

static int plant_http_append(const uint8_t *p, size_t n, void *arg)
{
  struct plant_http_body *b = arg;
  if (n > b->capacity - 1 - b->used) return -EMSGSIZE;
  memcpy(b->out + b->used, p, n);
  b->used += n;
  b->out[b->used] = '\0';
  return 0;
}

static int plant_http_decimal(const char *p, size_t *value)
{
  size_t v = 0;
  unsigned digits = 0;
  while (*p == ' ' || *p == '\t') p++;
  while (*p >= '0' && *p <= '9')
    {
      unsigned d = (unsigned)(*p++ - '0');
      if (v > (SIZE_MAX - d) / 10) return -EOVERFLOW;
      v = v * 10 + d;
      digits++;
    }
  while (*p == ' ' || *p == '\t') p++;
  if (!digits || *p) return -EPROTO;
  *value = v;
  return 0;
}

static int plant_http_read_response(int fd, char *out, size_t capacity,
                                    int *status_out)
{
  char line[512];
  uint8_t buf[256];
  size_t line_n = 0, header_n = 0, length = 0;
  int status = 0, have_length = 0, chunked = 0, in_body = 0;
  struct plant_chunked chunks = {0};
  struct plant_http_body body = {out, capacity, 0};
  if (!out || !capacity) return -EINVAL;
  out[0] = '\0';
  if (status_out) *status_out = 0;
  for (;;)
    {
      ssize_t n = recv(fd, buf, sizeof(buf), 0);
      if (n < 0)
        {
          if (errno == EINTR) continue;
          return -errno;
        }
      if (!n)
        {
          if (!in_body || chunked || (have_length && body.used != length))
            return -EPROTO;
          return (int)body.used;
        }
      size_t pos = 0;
      while (!in_body && pos < (size_t)n)
        {
          unsigned char c = buf[pos++];
          if (!c || ++header_n > 4096 || line_n >= sizeof(line) - 1)
            return -EMSGSIZE;
          line[line_n++] = (char)c;
          if (c != '\n') continue;
          if (line_n < 2 || line[line_n - 2] != '\r') return -EPROTO;
          line[line_n - 2] = '\0';
          if (!status)
            {
              /* Limit to HTTP/1.x, three status digits and a reason separator. */
              if (strncmp(line, "HTTP/1.", 7) ||
                  line[7] < '0' || line[7] > '9' || line[8] != ' ' ||
                  strlen(line) < 12 || line[9] < '1' || line[9] > '5' ||
                  line[10] < '0' || line[10] > '9' ||
                  line[11] < '0' || line[11] > '9' ||
                  (line[12] && line[12] != ' ')) return -EPROTO;
              status = (line[9] - '0') * 100 +
                       (line[10] - '0') * 10 + line[11] - '0';
            }
          else if (!line[0])
            {
              if (status < 200)
                {
                  if (status == 101 || have_length || chunked) return -EPROTO;
                  status = 0;
                }
              else
                {
                  if (status_out) *status_out = status;
                  if (chunked && have_length) return -EPROTO;
                  if (status == 204 || status == 304) return 0;
                  if (have_length && length >= capacity) return -EMSGSIZE;
                  if (have_length && !length) return 0;
                  in_body = 1;
                }
            }
          else
            {
              char *colon = strchr(line, ':');
              if (!colon || colon == line) return -EPROTO;
              *colon++ = '\0';
              while (*colon == ' ' || *colon == '\t') colon++;
              if (!strcasecmp(line, "Content-Length"))
                {
                  size_t value;
                  int rc = plant_http_decimal(colon, &value);
                  if (rc < 0) return rc;
                  if (have_length && value != length) return -EPROTO;
                  have_length = 1;
                  length = value;
                }
              else if (!strcasecmp(line, "Transfer-Encoding"))
                {
                  size_t len = strlen(colon);
                  while (len && (colon[len-1] == ' ' || colon[len-1] == '\t'))
                    colon[--len] = '\0';
                  if (chunked || strcasecmp(colon, "chunked")) return -EPROTO;
                  chunked = 1;
                }
            }
          line_n = 0;
        }
      if (in_body && pos < (size_t)n)
        {
          size_t take = (size_t)n - pos;
          int rc;
          if (chunked)
            {
              rc = plant_chunk_feed(&chunks, buf + pos, take,
                                    plant_http_append, &body);
              if (rc < 0) return rc;
              if (chunks.state == 5) return (int)body.used;
            }
          else
            {
              if (have_length && take > length - body.used)
                take = length - body.used;
              rc = plant_http_append(buf + pos, take, &body);
              if (rc < 0) return rc;
              if (have_length && body.used == length) return (int)body.used;
            }
        }
    }
}
#endif
