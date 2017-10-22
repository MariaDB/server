#ifndef __RCMSG_H__
#define __RCMSG_H__

#ifdef __cplusplus
extern "C" {
#endif

const char *GetMsgid(int id);
int GetRcString(int id, char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif /* __RCMSG_H__ */
