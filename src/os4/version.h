/* sftp-handler version; tools/build_os4.sh bumps SFH_REVISION. */
#ifndef SFH_VERSION_H
#define SFH_VERSION_H

#define SFH_VERSION  0
#define SFH_REVISION 15
#define SFH_DATE     "18.11.2026"

#define SFH_STR2(x) #x
#define SFH_STR(x)  SFH_STR2(x)
/* builds made off the main branch are betas (tools/build_os4.sh) */
#ifdef SFH_BETA
#define SFH_BETA_STR " beta"
#else
#define SFH_BETA_STR ""
#endif

#define SFH_VERNUM  SFH_STR(SFH_VERSION) "." SFH_STR(SFH_REVISION)
#define SFH_VERS    "sftp-handler " SFH_VERNUM SFH_BETA_STR
#define SFH_VERSTAG "\0$VER: sftp-handler " SFH_VERNUM " (" SFH_DATE ")" SFH_BETA_STR

#endif
