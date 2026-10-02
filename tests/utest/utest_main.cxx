// Windows: utest.h includes <Windows.h>. Without WIN32_LEAN_AND_MEAN the Windows SDK then pulls in the
// legacy <winsock.h>, which clashes with <winsock2.h> ("'sockaddr': 'struct' type redefinition").
// So: lean headers first, and <winsock2.h> before utest.h.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#endif


#include "utest.h"

UTEST_MAIN();