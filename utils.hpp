// https://stackoverflow.com/questions/17516855/portable-equivalent-of-debugbreak
#define BREAKPOINT				\
  asm("int $3")

#include <cstdlib>

// Divides integers x by y but rounds up
template <typename T>
inline T integerDivisionRoundingUp(T numer, T denom) {
  static_assert(std::is_integral<T>::value, "Integral required."); // https://en.cppreference.com/w/cpp/types/is_integral
  
  // Based on https://www.reddit.com/r/C_Programming/comments/gqpuef/how_to_round_up_the_answer_of_an_integer_division/
  return numer / denom + (numer % denom != 0 ? 1 : 0);
}
