// thebigs2 - temporary online/sign-in debugging helpers (TB2_ONLINE_DEBUG).
#pragma once

namespace tb2 {

// Dumps the game's online manager (DAT_825e34f0) sign-in state to the log.
void DumpOnlineManager();

// Broadcasts XN_SYS_SIGNINCHANGED (0x0A) to every notification listener.
void NudgeSigninChanged();

// Broadcasts the Xbox Live "connection changed" notification (0x02000001).
void NudgeLiveConnection();

}  // namespace tb2
