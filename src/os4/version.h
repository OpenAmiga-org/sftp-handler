/* sftp-handler version; tools/build_os4.sh bumps SFH_REVISION. */
#ifndef SFH_VERSION_H
#define SFH_VERSION_H

#define SFH_VERSION  0
#define SFH_REVISION 8
#define SFH_DATE     "15.11.2026"

#define SFH_STR2(x) #x
#define SFH_STR(x)  SFH_STR2(x)
#define SFH_VERS    "sftp-handler " SFH_STR(SFH_VERSION) "." SFH_STR(SFH_REVISION)
#define SFH_VERSTAG "\0$VER: " SFH_VERS " (" SFH_DATE ")"

#endif
