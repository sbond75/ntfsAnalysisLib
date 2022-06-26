// https://stackoverflow.com/questions/17516855/portable-equivalent-of-debugbreak
#define BREAKPOINT				\
  { if (debuggerIsAttached()) { asm("int $3"); } }

#include <cstdlib>

// Divides integers x by y but rounds up
template <typename T>
inline T integerDivisionRoundingUp(T numer, T denom) {
  static_assert(std::is_integral<T>::value, "Integral required."); // https://en.cppreference.com/w/cpp/types/is_integral
  
  // Based on https://www.reddit.com/r/C_Programming/comments/gqpuef/how_to_round_up_the_answer_of_an_integer_division/
  return numer / denom + (numer % denom != 0 ? 1 : 0);
}


// https://stackoverflow.com/questions/3596781/how-to-detect-if-the-current-process-is-being-run-by-gdb //

#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

bool debuggerIsAttached()
{
    char buf[4096];

    const int status_fd = ::open("/proc/self/status", O_RDONLY);
    if (status_fd == -1)
        return false;

    const ssize_t num_read = ::read(status_fd, buf, sizeof(buf) - 1);
    ::close(status_fd);

    if (num_read <= 0)
        return false;

    buf[num_read] = '\0';
    constexpr char tracerPidString[] = "TracerPid:";
    const auto tracer_pid_ptr = ::strstr(buf, tracerPidString);
    if (!tracer_pid_ptr)
        return false;

    for (const char* characterPtr = tracer_pid_ptr + sizeof(tracerPidString) - 1; characterPtr <= buf + num_read; ++characterPtr)
    {
        if (::isspace(*characterPtr))
            continue;
        else
            return ::isdigit(*characterPtr) != 0 && *characterPtr != '0';
    }

    return false;
}

// //
