// https://stackoverflow.com/questions/17516855/portable-equivalent-of-debugbreak
#define BREAKPOINT				\
  asm("int $3")

#include <cstdlib>

// Divides integers x by y but rounds up
template <typename T>
inline T integerDivisionRoundingUp(T numer, T denom) {
  static_assert(std::is_integral<T>::value, "Integral required."); // https://en.cppreference.com/w/cpp/types/is_integral
  
  // https://www.reddit.com/r/C_Programming/comments/gqpuef/how_to_round_up_the_answer_of_an_integer_division/
  // https://www.cplusplus.com/reference/cstdlib/div/ , https://en.cppreference.com/w/cpp/numeric/math/div
  // https://stackoverflow.com/questions/17854407/how-to-make-a-conditional-typedef-in-c
  //typedef typename std::conditional<std::is_unsigned<T>::value, uintmax_t, intmax_t>::type ConvertTo;
  using ConvertTo = intmax_t; // Just using signed since div() only seems to support signed anyway.. probably it is the same either way?
  auto d = std::div((ConvertTo)numer, (ConvertTo)denom);
  T result = d.quot + d.rem ? 1 : 0;
  return result;
}
