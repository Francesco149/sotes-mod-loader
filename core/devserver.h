// core/devserver.h — opt-in localhost control server for agents / tests (devserver=PORT in
// oss_loader.cfg, or env OSS_DEVSERVER=PORT).  Line protocol; Lua runs on the engine thread only.
#ifndef OSS_DEVSERVER_H
#define OSS_DEVSERVER_H
void devserver_start(void);
#endif
