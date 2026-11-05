#include <Logging.h>
#include <cstdarg>
#include <cstdio>

// The 1.2 snapshot declares this Print facade but does not define it. Keep
// legacy Serial call sites on the single IDF console owner for the Plus port.
MySerialImpl MySerialImpl::instance;
size_t MySerialImpl::write(uint8_t byte) { return write(&byte, 1); }
size_t MySerialImpl::write(const uint8_t* buffer, size_t size) {
  return buffer ? fwrite(buffer, 1, size, stdout) : 0;
}
void MySerialImpl::flush() { fflush(stdout); }
size_t MySerialImpl::printf(const char* format, ...) {
  char buffer[256];
  va_list arguments;
  va_start(arguments, format);
  const int length = vsnprintf(buffer, sizeof(buffer), format, arguments);
  va_end(arguments);
  if (length <= 0) return 0;
  const auto size = static_cast<size_t>(length) < sizeof(buffer) ? static_cast<size_t>(length) : sizeof(buffer) - 1;
  return write(reinterpret_cast<const uint8_t*>(buffer), size);
}
