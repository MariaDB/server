#ifndef PRINT_LOG_H
#define PRINT_LOG_H

#ifdef  __cplusplus
extern "C" {
#endif

void print_log_info(const char *fmt, ...);

void print_log_warning(const char *fmt, ...);

void print_log_error(const char *fmt, ...);

#ifdef  __cplusplus
}
#endif

#endif  /* PRINT_LOG_H */
