#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include "ap/ap.hpp" // https://github.com/arbitrary-precision/ap
#include <cassert>
#include <vector>
#include <variant>
#include "stdvisit_helpers.hpp"
#include "utils.hpp"
#include "tools.h"
#include <algorithm>
// https://www.cplusplus.com/reference/locale/wstring_convert/
#include <locale>         // std::wstring_convert
#include <codecvt>        // std::codecvt_utf8
#include <gmpxx.h> // C++ API for The GNU Multiple Precision Arithmetic Library (GMP)
#include <type_traits>

// Attempt to ensure little-endian CPU (big-endian CPU could be supported but would require using conversions when reading/writing to structs that represent on-disk data structures from NTFS, such as using https://man7.org/linux/man-pages/man3/endian.3.html )
// Based on https://stackoverflow.com/questions/4239993/determining-endianness-at-compile-time :
#include <endian.h> // Or try <sys/param.h>
#if defined(__BYTE_ORDER) && __BYTE_ORDER == __BIG_ENDIAN || \
    defined(__BIG_ENDIAN__) || \
    defined(__ARMEB__) || \
    defined(__THUMBEB__) || \
    defined(__AARCH64EB__) || \
    defined(_MIBSEB) || defined(__MIBSEB) || defined(__MIBSEB__)
// It's a big-endian target architecture
#error "Big-endian CPU not yet supported"
#elif defined(__BYTE_ORDER) && __BYTE_ORDER == __LITTLE_ENDIAN || \
    defined(__LITTLE_ENDIAN__) || \
    defined(__ARMEL__) || \
    defined(__THUMBEL__) || \
    defined(__AARCH64EL__) || \
    defined(_MIPSEL) || defined(__MIPSEL) || defined(__MIPSEL__)
// It's a little-endian target architecture
#else
#error "I don't know what architecture this is!"
#endif

// Lib //

// https://stackoverflow.com/questions/8152720/correct-way-to-inherit-from-stdexception
class UnhandledValue: public std::exception {
public:
  virtual char const* what() const noexcept { return "Unhandled value"; }
};

int _open(const char *pathname, int flags) {
  int fd = open(pathname, flags);
  if (fd == -1) {
    perror("open failed");
    throw errno;
  }
  return fd;
}
ssize_t _read(int fd, void* buf, size_t count) {
  ssize_t ret = read(fd, buf, count);
  if (ret == -1) {
    perror("read failed");
    throw errno;
  }
  else if (ret < count) {
    perror("read got too few bytes");
    throw errno;
  }
  return ret;
}
off_t _lseek(int fd, off_t offset, int whence) {
  // Get current offset
  off_t origOffsetFromStartOfFile = lseek(fd, 0, SEEK_CUR); // (Shouldn't change the offset in the file, but returns the offset from the start as lseek usually does.)
  if (origOffsetFromStartOfFile == -1) {
    perror("lseek to get current offset failed");
    throw errno;
  }
  
  off_t ret = lseek(fd, offset, whence);
  off_t desired;
  if (ret == -1) {
    perror("lseek failed");
    throw errno;
  }
  else if ((whence == SEEK_CUR && ret != (desired=origOffsetFromStartOfFile + offset)) ||
	   (whence == SEEK_SET && ret != (desired=offset))) {
    fprintf(stderr, "lseek didn't go to the expected offset: expected %jd but got %jd\n", (intmax_t)desired, (intmax_t)ret);
    throw errno;
  }
  return ret;
}
int _close(int fd) {
  int ret = close(fd);
  if (ret == -1) {
    perror("close failed");
    throw errno;
  }
  return ret;
}

// //

// To force compiler to use 1 byte packaging
#pragma pack(1)

// Most of this is based on the descriptions on https://www.cse.scu.edu/~tschwarz/coen252_07Fall/Lectures/NTFS.html

enum MFTEntryFlags: uint16_t {
  RecordInUse = 0x01, // If this is set, the entry is *not* deleted. If it is not set, the record can be reused because it points to a deleted file! ("When a file is created, an unused FILE record can be re-used for it, but its sequence number is [if non-zero] incremented by one [and skipping 0]. This mechanism allows NTFS to check that file references don't point to deleted files." -- ntfsdoc-0.6/concepts/file_record.html )
  Directory = 0x02
};

typedef ap_uint<48> uint48;

template <typename T>
struct ArrayWithLengthIterator {
  T* val;
  size_t entriesRemainingIncludingThisOne; // 0 means this is considered an invalid ArrayWithLengthIterator.

  ArrayWithLengthIterator<T> operator++() {
    auto ret = *this;
    val = val+1;
    entriesRemainingIncludingThisOne = entriesRemainingIncludingThisOne == 0 ? 0 : entriesRemainingIncludingThisOne-1;
    return ret;
  }
    
  T& operator* () const
  {
    if (entriesRemainingIncludingThisOne == 0) {
      printf("No more entries left in ArrayWithLengthIterator");
      throw entriesRemainingIncludingThisOne;
    }
    return *val;
  }

  T* operator-> () const
  {
    if (entriesRemainingIncludingThisOne == 0) {
      printf("No more entries left in ArrayWithLengthIterator");
      throw entriesRemainingIncludingThisOne;
    }
    return val;
  }
};
template <typename T>
bool operator==(ArrayWithLengthIterator<T> o1, ArrayWithLengthIterator<T> o2) {
  return o1.val == o2.val;
}
template <typename T>
bool operator!=(ArrayWithLengthIterator<T> o1, ArrayWithLengthIterator<T> o2) {
  return !(o1 == o2);
}

template <typename T>
struct ArrayWithLength_base {
  T* array;
  size_t length;

  // Returns the length of the array in bytes.
  size_t byteLength() const {
    return length * sizeof(T);
  }

  ArrayWithLengthIterator<T> begin() { return {array, length}; }
  ArrayWithLengthIterator<T> end() { return {array+length, 0}; }
};
template <typename T>
struct ArrayWithLength: ArrayWithLength_base<T> {};

// https://stackoverflow.com/questions/3477525/is-it-possible-to-use-a-c-smart-pointers-together-with-cs-malloc
struct free_delete
{
    void operator()(void* x) { free(x); }
};
template <typename T>
using unique_free = std::unique_ptr<T, free_delete>;

// For GMP library-allocated buffers
struct free_mp
{
  size_t wordCount, wordSize;
  void operator()(void* outRaw) {
    // https://stackoverflow.com/questions/51601666/gmp-store-64-bit-interger-in-mpz-t-mpz-class-and-get-64-bit-integers-back
    // Free the allocated memory by mpz_export
    void (*freeFunction)(void*, size_t);
    mp_get_memory_functions(nullptr, nullptr, &freeFunction);
    freeFunction(outRaw, wordCount * wordSize);
  }
};
template <typename T>
using unique_free_mp = std::unique_ptr<T, free_mp>;

// https://stackoverflow.com/questions/41744559/is-this-a-bug-of-gcc : {"
// Also see LWG issue 721 [ http://www.open-std.org/jtc1/sc22/wg21/docs/lwg-closed.html#721 ] (decided as Not A Defect).
//   "This is a regrettable consequence of the original design of the facet."
// The defect report also has an example of how to construct such object:
// "}
template<class I, class E, class S>
struct codecvt : std::codecvt<I, E, S> { ~codecvt() {} };
// Usage example: `std::wstring_convert<codecvt<wchar_t, char, std::mbstate_t> >;`

// https://stackoverflow.com/questions/27453449/c-template-partial-specialization-with-inheritance
template <>
struct ArrayWithLength<uint16_t>: ArrayWithLength_base<uint16_t> {
  // unique_free<wchar_t> wstr() const {
  //   static_assert(sizeof(wchar_t) == sizeof(uint16_t)); // If this fails, use another type instead of wchar_t
    
  //   wchar_t* res = malloc(byteLength() + (1*sizeof(wchar_t) /*for null terminator we will add*/));
  //   memcpy(res, array, byteLength());
  //   res[byteLength()] = L'\0'; // Null terminate it
  //   return res;
  // }

  std::string to_string() const {
    // Demo: std::string s = u8"Hello, World!";
    //const char* s = (const char*)array;
    // #include <codecvt>
    // `std::codecvt<char16_t, char, std::mbstate_t>` : "conversion between UTF-16 and UTF-8" ( https://en.cppreference.com/w/cpp/locale/codecvt )
    std::wstring_convert<codecvt<char16_t,char,std::mbstate_t>,char16_t> convert; // https://stackoverflow.com/questions/11086183/encode-decode-stdstring-to-utf-16
    //std::u16string u16 = convert.from_bytes(s, (const char*)((uint8_t*)array + byteLength())); // Args are: first, last (last is exclusive)
    //std::string u8 = convert.to_bytes(u16);

    // https://gist.github.com/gchudnov/c1ba72d45e394180e22f
    std::string u8 = convert.to_bytes((const char16_t*)array, (const char16_t*)((uint8_t*)array + byteLength()));
    
    // Fails: static_assert(sizeof(wchar_t) == sizeof(uint16_t)); // If this fails, use another type instead of wchar_t
    //return std::wstring(array, byteLength());

    return u8;
  }

};

enum AttributeTypeIdentifier: uint32_t {
  STANDARD_INFORMATION = 0x10,
  ATTRIBUTE_LIST = 0x20,
  FILE_NAME = 0x30,
  VOLUME_VERSION = 0x40, // Windows NT
  OBJECT_ID = 0x40, // Windows 2000
  SECURITY_DESCRIPTOR = 0x50,
  VOLUME_NAME = 0x60,
  VOLUME_INFORMATION = 0x70,
  DATA = 0x80,
  INDEX_ROOT = 0x90,
  INDEX_ALLOCATION = 0xA0,
  BITMAP = 0xB0,
  SYMBOLIC_LINK = 0xC0, // Windows NT
  REPARSE_POINT = 0xC0, // Windows 2000
  EA_INFORMATION = 0xD0,
  EA = 0xE0,
  PROPERTY_SET = 0xF0, // Windows NT
  LOGGED_UTILITY_STREAM = 0x100 // Windows 2000
};

// Notes: "Only the data attribute can be compressed, or sparse, and only when it is non-resident." + "Although the compression flag is stored in the header, it does not affect the size of the header." ( ntfsdoc-0.6/concepts/attribute_header.html )
enum AttributeFlags: uint16_t {
  AttributeFlags_Compressed = 0x0001,
  AttributeFlags_Encrypted = 0x4000,
  AttributeFlags_Sparse = 0x8000
};

struct AttributeBase {
  AttributeTypeIdentifier typeIdentifier; // "The attribute type identifier determines also the layout of the contents."
  uint32_t attributeLength; // (determines the location of next attribute)
  uint8_t nonResidentFlag;
  uint8_t lengthOfName; // (Optional, if a name is present then this is a "named attribute" ( ntfsdoc-0.6/concepts/attribute_header.html ))
  uint16_t offsetToName; // (Optional, same situation as the above)
  AttributeFlags flags;
  uint16_t attributeIdentifier; // "Each attribute has a unique identifier" ( ntfsdoc-0.6/concepts/attribute_header.html ) + "Every Attribute in every FILE Record has an Attribute Id. This Id is unique within the FILE Record and is used to maintain data integrity." ( ntfsdoc-0.6/concepts/attribute_id.html )
  
  // Returns the attribute name or nullptr if this is not a named attribute.
  ArrayWithLength<uint16_t> name() const {
    if (lengthOfName != 0) {
      assert(offsetToName != 0);
      return {(uint16_t*)((uint8_t*)this + offsetToName), lengthOfName};
    }
    else {
      //assert(offsetToName == 0);
      return {nullptr, 0};
    }
  }
};

// "The time values are given in 100 nanoseconds since January 1, 1601, UTC."
struct Times { 
  uint64_t cTime; // File creation time
  uint64_t aTime; // File altered time
  uint64_t mTime; // MFT changed time
  uint64_t rTime; // File read time
};
struct StandardInformation {
  Times times;
  uint32_t dosPermissions;
  uint32_t maximumNumberOfVersions;
  uint32_t versionNumber;
  uint32_t classID;
  uint32_t ownerID;
  uint32_t securityID;
  uint64_t quotaChanged;
  uint64_t usn; // Update Sequence Number (USN)
};
enum FileNameFlags: uint32_t {
  ReadOnly = 0x0001,
  Hidden = 0x0002,
  System = 0x0004,
  Archive = 0x0020,
  Device = 0x0040,
  Normal = 0x0080,
  Temporary = 0x0100,
  SparseFile = 0x0200,
  ReparsePoint = 0x0400,
  Compressed = 0x0800,
  Offline = 0x1000,
  NotContentIndexed = 0x2000,
  Encrypted = 0x4000,
  FileNameFlags_Directory = 0x10000000, // (copy from corresponding bit in MFT record)
  IndexView = 0x20000000 // (copy from corresponding bit in MFT record)
};
// Misc note: "NTFS implements POSIX-style Hard Links by creating a file with several Filename Attributes. Each Filename Attribute has its own details and parent [fileReferenceToParentDirectory]. When a Hard Linked file is deleted, its filename is removed from the MFT Record. When the last link is removed, then the file is really deleted." ( ntfsdoc-0.6/attributes/file_name.html#file_flags )
struct FileName {
  // "N.B. All fields, except the parent directory, are only updated when the filename is changed. Until then, they just become out of date. $STANDARD_INFORMATION Attribute, however, will always be kept up-to-date." ( ntfsdoc-0.6/attributes/file_name.html#file_flags )
  
  uint64_t fileReferenceToParentDirectory; // This is a "file reference" which has a specific meaning: "A reference consists of a 48-bit index into the mft [numberOfThisMFTRecord] and a 16-bit sequence
  // number [sequenceNumber] used to detect stale references." ( ntfsdoc-0.6/concepts/file_reference.html )

  Times times;
  uint64_t allocatedSizeOfFile; // "The allocated size of a file is the amount of disk space the file is taking up. It will be a multiple of the cluster size. The real size of the file is the size of the unnamed data attribute. This is the number that will appear in a directory listing." ( ntfsdoc-0.6/attributes/file_name.html )
  uint64_t realSizeOfFile; // "N.B. The Real Size is only present if the Starting VCN [NonResidentAttribute.startingVirtualClusterNumber] is zero. See the Standard Attribute Header [struct NonResidentAttribute or more generally ResidentAttribute] for more information." ( ntfsdoc-0.6/attributes/file_name.html )
  FileNameFlags flags;
  uint32_t usedBy_EAs_andReparse; // "N.B. If the file has EAs (Extended Attributes), then the EA Field will contain the size of buffer needed." + "N.B. If the file is a Reparse Point [FileNameFlags::ReparsePoint], then the Reparse Field will give its type." ( ntfsdoc-0.6/attributes/file_name.html#file_flags )
  //uint32_t securityID; // <-- doesn't exist?
  uint8_t filenameLengthInUnicodeCharacters;
  uint8_t filenameNamespace;

  ArrayWithLength<uint16_t> fileNameInUnicode /*and 16-bit characters*/ () {
    return {(uint16_t*)((uint8_t*)this + sizeof(FileName)), filenameLengthInUnicodeCharacters};
  }
};
static_assert(offsetof(FileName, allocatedSizeOfFile) == 0x28);
static_assert(offsetof(FileName, usedBy_EAs_andReparse) == 0x3c);
static_assert(sizeof(FileName) == 0x42); // Based on the filename in Unicode being at 0x42 according to ntfsdoc-0.6/attributes/file_name.html where it says "File name in Unicode (not null terminated)"
struct Data {
  // Contains anything! For a ResidentAttribute containing this, use `sizeOfContent` to tell how long this Data is. TODO: NonResidentAttribute.
};
using AttributeContent = std::variant<StandardInformation*, FileName*, Data*>; // Note: there are more than just these
// Contains `ptr` which will be freed and the superclass which holds type info for the `ptr`.
class AttributeContentWithFreer: public AttributeContent {
protected:
  // https://stackoverflow.com/questions/8967521/class-template-with-template-class-friend-whats-really-going-on-here
  template<typename U>
  friend class TypedAttributeContentWithFreer;
  
  unique_free<void*> ptr;
public:
  // Original:
  //   template <typename T>
  //   AttributeContentWithFreer(unique_free<T>&& ptr_): ptr((void**)ptr_.release()), AttributeContent(ptr_.get()) {}
  // Should be a constructor but having a templated constructor apparently causes "you just can't call it ever." ( https://stackoverflow.com/questions/3960849/c-template-constructor )
  template <typename T>
  static AttributeContentWithFreer make(unique_free<T>&& ptr_) {
    AttributeContentWithFreer this_(AttributeContent(ptr_.get()));
    this_.ptr.reset((void**)ptr_.release());
    //return std::move(this_);
    return AttributeContentWithFreer::make<T>(std::move(this_));
  }

  // Original:
  //  template <typename T>
  //  AttributeContentWithFreer(AttributeContentWithFreer&& other): ptr(other.ptr.release()), AttributeContent(std::get<T*>(other)) {}
  // Should be a constructor but same issue as above.
  template <typename T>
  static AttributeContentWithFreer make(AttributeContentWithFreer&& other) {
    AttributeContentWithFreer this_((AttributeContent)other);
    this_.ptr.reset((void**)other.ptr.release());
    return std::move(this_);
  }

  AttributeContentWithFreer(const AttributeContentWithFreer& other) = delete;
  
  AttributeContentWithFreer(const AttributeContent&& other): ptr(), AttributeContent(other) {}
  
  AttributeContentWithFreer(const AttributeContent& other): ptr(), AttributeContent(other) {}
};
// Convenience version of the above.
template <typename T>
class TypedAttributeContentWithFreer {
protected:
  std::optional<AttributeContent> ac;
  unique_free<T> ptr;
public:
  TypedAttributeContentWithFreer(): ptr(), ac() {}
  
  TypedAttributeContentWithFreer(unique_free<T>&& ptr_): ptr(ptr_.release()), ac(ptr_.get()) {}

  TypedAttributeContentWithFreer(TypedAttributeContentWithFreer&& other): ptr(other.ptr.release()) {
    if (other.ac.has_value()) {
      ac = std::get<T*>(*other.ac);
    }
  }

  TypedAttributeContentWithFreer(const TypedAttributeContentWithFreer& other) = delete;
  
  TypedAttributeContentWithFreer(const AttributeContentWithFreer& other) = delete;
  
  TypedAttributeContentWithFreer(const AttributeContent&& other): ptr(), ac(other) {}
  
  TypedAttributeContentWithFreer(AttributeContentWithFreer&& other): ptr((T*)other.ptr.release()), ac(other) {}
  
  TypedAttributeContentWithFreer(const AttributeContent& other): ptr(), ac(other) {}
  
  // Similar interface to std::unique_ptr<T> //

  T* get() const { return ac.has_value() ? std::get<T*>(*ac) : nullptr; }
  bool isMalloced() const { return ptr.get() != nullptr; }
  
  T& operator* () const
  {
    return *get();
  }

  T* operator-> () const
  {
    return get();
  }
};
struct RunList; struct NTFS;

#pragma pack()
// Non-NTFS-specific struct
struct MyDataRun {
  size_t offset; // Offset in clusters from the start of the volume *or* previous data run's start if there is a previous one.
  size_t length; // Length in clusters of this run. If this is zero, ignore it.
};
#pragma pack(1)
#pragma pack()
// Non-NTFS-specific struct
struct MyDataRuns {
  std::vector<MyDataRun> dataRuns;
  bool hasMore; // Whether the last run has more data to it but it wasn't loaded, or there are more runs to be loaded but they weren't loaded.

  // Loads data from the dataRuns' specified offsets and lengths. See the definition of this function for more information.
  unique_free<void*> load(size_t bufOffset, void* buf, size_t amountToLoad, int fd, const NTFS* ntfs, bool* out_moreNeeded, ssize_t* out_more) const;
};
#pragma pack(1)

struct LazilyLoaded {
  RunList* runList; // The "lazy loader"

  // Using `runList`, "loads" (doesn't actually read from disk though) MyDataRuns up to and including totalOffsetFromStartInClusters (tip to specify in bytes: try passing in totalOffsetFromStartInClusters = x / `NTFS.bytesPerCluster()` where x is the number of bytes to load up to (and is a multiple of bytesPerCluster() -- round up to it if needed)). If there is more available but it isn't loaded, the returned MyDataRuns object will have hasMore set to true. You can call this function again with a larger totalOffsetFromStartInClusters to fix this.
  MyDataRuns loadUpTo(size_t totalOffsetFromStartInClusters) const;
};
//using NonResidentAttributeContent = LazilyLoaded;

// Resident = in this MFT. These have a different structure from non-resident ones.
struct ResidentAttribute {
  AttributeBase base;
  uint32_t sizeOfContent;
  uint16_t offsetToContent;
  uint8_t indexedFlag; // ntfsdoc-0.6/concepts/attribute_header.html
  char padding[1]; // ntfsdoc-0.6/concepts/attribute_header.html

  std::pair<AttributeContent, std::optional<MyDataRuns> /*placeholder, will be empty*/> content(.../*<--placeholder for std::visit, ignore this*/) const {
    uint8_t* contentPtr = (uint8_t*)this + offsetToContent;
    switch (base.typeIdentifier) {
    case STANDARD_INFORMATION:
      return std::make_pair((StandardInformation*)contentPtr, std::optional<MyDataRuns>());
    case FILE_NAME:
      return std::make_pair((FileName*)contentPtr, std::optional<MyDataRuns>());
    default:
      throw UnhandledValue();
    }
  }
};

// Wrapper for a GMP multi-precision integer ("z").
struct MPZWrapper {
  mpz_t z;
  bool initted;
  MPZWrapper() { mpz_init(z); initted = true; }
  MPZWrapper(mpz_t&& z_) { memcpy(&z, &z_, sizeof(mpz_t)); initted = true; } // Takes ownership
  MPZWrapper(MPZWrapper&& o) { memcpy(&z, &o.z, sizeof(mpz_t)); initted = true; o.initted = false; }
  MPZWrapper(const MPZWrapper& o) {
    if (o.initted) {
      mpz_init(z);
      initted = true;
      mpz_set(z, o.z); // Copy it over from `o`
    }
    else {
      initted = false;
    }
  }
  ~MPZWrapper() { if (initted) mpz_clear(z); /* Free it */ }

  MPZWrapper& operator=(const MPZWrapper& o) {
    if (o.initted) {
      if (!initted) mpz_init(z);
      initted = true;
      mpz_set(z, o.z); // Copy it over from `o`
    }
    else {
      if (initted) mpz_clear(z); /* Free it */
      initted = false;
    }
    return *this;
  }

  MPZWrapper(const uint8_t* source, size_t length) {
    mpz_init(z);
    initted = true;
    
    // https://gmplib.org/manual/Useful-Macros-and-Constants
    //static_assert(mp_bits_per_limb == sizeof(mp_limb_t) * CHAR_BIT); // Untested
    // https://gmplib.org/manual/Initializing-Integers
    //mpz_init2(z, (length + sizeof(mp_limb_t)) * CHAR_BIT); // CHAR_BIT is bits in a byte
    // https://gmplib.org/manual/Integer-Import-and-Export#Integer-Import-and-Export , https://stackoverflow.com/questions/6683773/how-to-initialize-a-mpz-t-in-gmp-with-a-1024-bit-number-from-a-character-string
    printf("MPZWrapper::MPZWrapper: importing: "); DumpHex(source, length);
    mpz_import(z, length /*count (in words)*/, -1 /*order*/, sizeof(uint8_t) /*each word's size in bytes*/, 0 /*endian*/, 0 /*nails*/, source); // "There is no sign taken from the data, rop will simply be a positive integer. An application can handle any sign itself, and apply it for instance with mpz_neg." ( https://gmplib.org/manual/Integer-Import-and-Export )
    gmp_printf ("MPZWrapper::MPZWrapper: imported mpz_t: %Zu\n", z);
  }

  size_t toSizeT() const {
    assert(initted);
    
    //if (mpz_fits_ulong_p(z.z)) { // Then this fits in an unsigned long int.
    //static_assert(sizeof(unsigned long int) >= sizeof(size_t));

    size_t bytesNeededForZ = integerDivisionRoundingUp(mpz_sizeinbase(z, 2), (size_t)CHAR_BIT); // Convert bits to bytes, rounding up so we know it can be stored
    printf("MPZWrapper::toSizeT: bytesNeededForZ: %ju, sizeof(size_t): %ju\n", (uintmax_t)bytesNeededForZ, (uintmax_t)sizeof(size_t));
    assert(bytesNeededForZ <= sizeof(size_t));
    //size_t builtInSizeT;
    //mpz_import(z, 1, 1, sizeof(size_t), 0, 0, &builtInSizeT);

    constexpr size_t wordSize = 1;
    size_t wordCount;
    constexpr size_t maxWordCount = sizeof(size_t);
    void* buf = (void*)mpz_export(nullptr, &wordCount, -1, wordSize, 0, 0, z); // "The sign of `op` [`z`] is ignored, just the absolute value is exported. An application can use mpz_sgn to get the sign and handle it as desired. (see Section 5.10 [Integer Comparisons] [ https://gmplib.org/manual/Integer-Comparisons.html ], page 39)" ( https://gmplib.org/manual/Integer-Import-and-Export + pdf manual )
    unique_free_mp<void*> outRaw((void**)buf,
				 free_mp{wordCount, wordSize});
    printf("MPZWrapper::toSizeT: wordCount = %ju, maxWordCount = %ju\n", (uintmax_t)wordCount, (uintmax_t)maxWordCount);
    assert(wordCount <= maxWordCount); // Make sure that our integer type can still hold the value
    printf("MPZWrapper::toSizeT: result before realloc: "); DumpHex(outRaw.get(), wordCount);

    void* (*reallocFunction)(void*, size_t, size_t);
    mp_get_memory_functions(nullptr, &reallocFunction, nullptr);
    buf = reallocFunction(buf, wordCount * wordSize, maxWordCount * wordSize); // buf, old size, new size
    // Zero out the rest
    memset((uint8_t*)buf + (wordCount * wordSize), 0, (maxWordCount * wordSize - wordCount * wordSize)); 
    printf("MPZWrapper::toSizeT: result after realloc: "); DumpHex(outRaw.get(), maxWordCount);
    
    const size_t out = *(size_t*)(outRaw.get());
    return out;
  }
};

// This struct describes ntfsdoc-0.6/concepts/data_runs.html
struct RunList {
  uint8_t header; // This header tells you (via the two nibbles of this header byte) how large the `offset()` and `length()` values are, respectively. After these offset() and length() values is 0x00, a null byte to terminate the RunList.

  size_t sizeOfLength() const {
    return header & 0x0f; // `header` = 0xXY (in hex) where Y is the size of `length()` and X is the size of `offset()`
  }

  size_t sizeOfOffset() const {
    return header >> 4;
  }

  // Returns the length of the clusters pointed to by this RunList, in clusters.
  MPZWrapper length() const {
    uint8_t* value = (uint8_t*)this + sizeof(RunList().header);
    size_t length = sizeOfLength();
    printf("RunList::length(): "); DumpHex(value, length);
    MPZWrapper z(value, length);
    return std::move(z);
  }

  // Returns the offset of the clusters pointed to by this RunList, in LCNs (logical cluster numbers). This offset is from the start of the NTFS volume *if* this is the first entry in the RunList; otherwise, this is the offset from the `offset()` of the previous entry in the RunList.
  MPZWrapper offset() const {
    uint8_t* value = (uint8_t*)this + sizeof(RunList().header) + sizeOfLength();
    size_t length = sizeOfOffset();
    printf("RunList::offset(): "); DumpHex(value, length);
    MPZWrapper z(value, length);
    return std::move(z);
  }

  // Returns the next entry of this RunList, or nullptr if this is the last one.
  RunList* next() const {
    uint8_t* value = (uint8_t*)this + sizeof(RunList().header) + sizeOfLength() + sizeOfOffset();
    if (*value == 0x00) {
      return nullptr;
    }
    return (RunList*)value;
  }
};

MyDataRuns LazilyLoaded::loadUpTo(size_t totalOffsetFromStartInClusters) const {
  // Accumulate offsets
  MyDataRuns dataRuns;
  size_t counter = 0, offsetCounter = 0;
  for (RunList* rl = runList; rl != nullptr; rl = rl->next()) {
    MPZWrapper z = runList->offset();
    size_t offset = z.toSizeT();
    z = runList->length();
    size_t length = z.toSizeT();
    printf("LazilyLoaded::loadUpTo: processing RunList: offset size = %ju, length size = %ju, offset = %ju, length = %ju\n", (uintmax_t)runList->sizeOfOffset(), (uintmax_t)runList->sizeOfLength(), (uintmax_t)offset, (uintmax_t)length);

    counter += length;
    offsetCounter += offset;
    dataRuns.dataRuns.push_back({
	.offset = offset,
	.length = counter >= totalOffsetFromStartInClusters ? counter - totalOffsetFromStartInClusters : length
      });
    dataRuns.hasMore = counter > totalOffsetFromStartInClusters; // || rl->next() != nullptr;
    if (counter >= totalOffsetFromStartInClusters) {
      // Done loading
      printf("LazilyLoaded::loadUpTo: done loading: counter %ju, totalOffsetFromStartInClusters %ju, rl->next() %p\n", (uintmax_t)counter, (uintmax_t)totalOffsetFromStartInClusters, rl->next()); // if (rl->next() != nullptr || counter > totalOffsetFromStartInClusters) then more is left to load!
      break;
    }
  }

  return dataRuns;
}

// "non-resident attributes need to describe an arbitrary number of cluster runs, consecutive clusters that they occupy."
struct NonResidentAttribute {
  AttributeBase base;
  uint64_t startingVirtualClusterNumberOfTheDataRuns;
  uint64_t endingVirtualClusterNumberOfTheDataRuns;
  uint16_t offsetToTheRunList; // aka the "[list of stuff that points to the] data runs"
  uint16_t compressionUnitSize; // "[Actual?] compression unit size = 2^x clusters [where x is probably compressionUnitSize]. 0 implies uncompressed" ( ntfsdoc-0.6/concepts/attribute_header.html )
  uint32_t unused;
  uint64_t allocatedSizeOfTheAttributeContent; // "This is the attribute size rounded up to the cluster size" ( ntfsdoc-0.6/concepts/attribute_header.html )
  uint64_t actualSizeOfTheAttributeContent;
  uint64_t initializedSizeOfTheAttributeContent; // "Compressed data size." ( ntfsdoc-0.6/concepts/attribute_header.html )

  std::pair<AttributeContentWithFreer, std::optional<MyDataRuns>> content(size_t limitToLoad, bool* out_moreNeeded, ssize_t* out_more, int fd, const NTFS* ntfs) const;
};

using Attribute = std::variant<ResidentAttribute*, NonResidentAttribute*>;
Attribute makeAttribute(AttributeBase* base) {
  switch (base->nonResidentFlag) {
  case 0:
    return (ResidentAttribute*)base;
  case 1:
    return (NonResidentAttribute*)base;
  default:
    throw UnhandledValue();
  }
}

// An entry within the MFT.
struct MFTRecord {
  char magicNumber[4]; // "FILE" (or, if the entry is unusable, we would find it marked as "BAAD").
  uint16_t updateSequenceOffset;
  uint16_t numEntriesInFixupArray; // Fixup array = update sequence (synonymns).  // This is the number of entries where an entry is a single 16 bit value.
  uint64_t logFileSequenceNumber; // (LSN)  // "Each MFT record is addressed by a 48 bit MFT entry value [is simply the 0-based index of this record; an "entry index"].The first entry has address 0. Each MFT entry has a 16 bit sequence number that is incremented when the entry is allocated. MFT entry value and sequence number combined yield 64b [bit] file reference address."  // "This is changed every time the record is modified." ( ntfsdoc-0.6/concepts/file_record.html )
  uint16_t sequenceNumber; // Says how many times this entry has been used.  // "N.B. The increment (skipping zero) is done when the file is deleted." + "N.B. If this is set to zero it is left as zero." ( ntfsdoc-0.6/concepts/file_record.html )
  uint16_t hardLinkCount; // "The hard link count is the number of directory entries that reference this record."
  uint16_t offsetToFirstAttribute; // *useful*
  MFTEntryFlags flags;
  uint32_t usedSizeOfMFTEntry;
  uint32_t allocatedSizeOfMFTEntry;
  uint64_t fileReferenceToTheBase_FILE_record; // "MFT entries could be larger than fit into the normal space. In this case, the MFT entry will start in the base MFT record and continued in an extension record." If the file reference to the base file entry is 0x 00 00 00 00 00 00 00 00 then this is a base record. Were it not so, then this field would contain a reference to the base MFT record.
  uint16_t nextAttributeID; // This is the "next attribute ID" in the sense that it is the next attribute ID to place into this MFTRecord *if* you are adding a new attribute entry I think. Since the attributes are in ascending order by ID apparently. Anyway, main point is that numAttributes() is based on this.       // ntfsdoc-0.6/concepts/attribute_id.html : {"
  // Next Attribute Id
  //     The Attribute Id that will be assigned to the next Attribute added to this MFT Record.
  //     N.B. Incremented each time it is used.
  //     N.B. Every time the MFT Record is reused this Id is set to zero.
  //     N.B. The first instance number is always 0.
  // "}
  char padding10[2]; // "Align to 4B boundary" on Windows XP
  // [I think this is this but not sure:] The "entry value" or "entry number" for this MFTRecord. This is just the 0-based index of this record basically.
  uint32_t numberOfThisMFTRecord; // On Windows XP
  char attributesAndFixupValue[0x1000-0x30]; // Attributes and fixup value

  size_t totalSize() const {
    // Wrong size presumably because the fixupArray() is in there too: return offsetof(MFTRecord, attributesAndFixupValue) + sizeOfAllAttributes() + sizeof(0xffffffff /*end of attributes list marker*/);

    size_t ret1 = offsetof(MFTRecord, attributesAndFixupValue) + fixupArray().byteLength() + sizeOfAllAttributes() + sizeof((uint32_t)0xffffffff /*end of attributes list marker*/);

    // NOTE: there seems to also be an extra 0xffff at the end of the record? I will include this for testing..:
    return ret1 + sizeof((uint16_t)0xffff);
  }

  void hexDump() const {
    auto size = totalSize();
    DumpHex(this, size);
    printf("  \tSize: %ju\n", (uintmax_t)size);
  }

  static const std::vector<const char*> possibleMagicNumbers;

  std::pair<MFTRecord* /*a pointer within the void* buffer*/,
	    unique_free<void*> /*the new buffer for use with `mdr`*/>
  next(const MyDataRuns& mdr, size_t amountAlreadyLoadedFromMDR, void* bufForMDR /*must be a malloc()'ed buffer*/,
       int fd, const NTFS* ntfs) const;

  // Returns the sum of all attributes' sizes.
  size_t sizeOfAllAttributes() const {
    size_t numAttrs = numAttributes();
    AttributeBase* currentAttr = (AttributeBase*)((uint8_t*)this + offsetToFirstAttribute);
    size_t acc = 0;
    for (size_t i = 0; i < numAttrs; i++) {
      acc += currentAttr->attributeLength;
      currentAttr = (AttributeBase*)((uint8_t*)currentAttr + currentAttr->attributeLength);
    }
    return acc;
  }
  
  size_t numAttributes() const {
    size_t retval = nextAttributeID - 1;
    
    // Find end marker
    size_t counter = 0;
    AttributeBase* currentAttr = (AttributeBase*)((uint8_t*)this + offsetToFirstAttribute);
    while (currentAttr->typeIdentifier != 0xffffffff // The end marker for attribute list
	     ) {
      if (!(counter < retval)) {
	printf("numAttributes: !( counter < retval)\n");
	break;
      }
      
      currentAttr = (AttributeBase*)((uint8_t*)currentAttr + currentAttr->attributeLength);
      counter++;
    };

    printf("numAttributes: counter %ju, retval %ju\n", (uintmax_t)counter, (uintmax_t)retval);
    //assert(counter == retval);
    //return retval;

    return counter;
  }

  bool isBaseRecord() const {
    return fileReferenceToTheBase_FILE_record == 0;
  }

  // TODO: wrong? what about fileReferenceToTheBase_FILE_record ?        :
  // Should equal fileReferenceToTheBase_FILE_record if volume is consistent.
  // "A reference consists of a 48-bit index into the mft [numberOfThisMFTRecord] and a 16-bit sequence
  // number [sequenceNumber] used to detect stale references." ( ntfsdoc-0.6/concepts/file_reference.html )
  uint64_t computedFileReferenceAddress() const {
    return ((uint64_t)sequenceNumber << 48) | (uint64_t)numberOfThisMFTRecord; // Based on explanation and figure under "LSN, File Reference Address" on https://www.cse.scu.edu/~tschwarz/coen252_07Fall/Lectures/NTFS.html
  }

  uint16_t updateSequenceNumber() const {
    return *(uint16_t*)((uint8_t*)this + updateSequenceOffset);
  }

  // This array contains actual values to be placed at the last 16-bit word of each sector in this record. (The values in the last 16-bit word of each sector *are* the updateSequenceNumber() unless you change it to what it should be in memory, which is the corresponding value from this fixupArray().)
  ArrayWithLength<uint16_t> fixupArray() const {
    return {(uint16_t*)((uint8_t*)this + updateSequenceOffset), numEntriesInFixupArray};
  }

  // Mutates `this`.
  void applyFixup(uint16_t bytesPerSector) {
    auto arr = fixupArray();
    // ntfsdoc-0.6/concepts/fixup.html
    // Last 2 bytes of each sector (each sector is of size 512 bytes usually) must be compared with the updateSequenceNumber() and replaced with the corresponding index from fixupArray().
    uint8_t* sectorIterator = (uint8_t*)this + bytesPerSector - sizeof(uint16_t);
    auto usn = updateSequenceNumber();
    for (auto val : arr) {
      if (sectorIterator > (uint8_t*)this + usedSizeOfMFTEntry) {
	printf("applyFixup: sectorIterator is past usedSizeOfMFTEntry\n");
	break;
      }
      
      uint16_t* valPtr = (uint16_t*)sectorIterator;
      printf("applyFixup: %ju should be usn %ju", (uintmax_t)*valPtr, (uintmax_t)usn);
      fflush(stdout);
      assert(*valPtr == usn);
      printf("; %ju -> %ju\n", (uintmax_t)*valPtr, (uintmax_t)val);
      *valPtr = val;
      sectorIterator += bytesPerSector;
    }
  }

  std::vector<Attribute> attributes() const {
    std::vector<Attribute> ret;
    AttributeBase* currentAttr = (AttributeBase*)((uint8_t*)this + offsetToFirstAttribute);

    // TODO: what if there are no attributes? this is a failsafe: (maybe return empty array instead but I'm not sure if 0 means this) :
    assert(offsetToFirstAttribute!=0);

    ret.reserve(numAttributes());
    for (size_t i = 0; i < numAttributes(); i++) {
      Attribute attr = makeAttribute(currentAttr);
      ret.push_back(attr);
      printf("MFTRecord::attributes: found attribute with type %#jx and offset %#jx from the start of the MFTRecord\n", (uintmax_t)currentAttr->typeIdentifier, (uintmax_t)((uint8_t*)currentAttr - (uint8_t*)this));
      currentAttr = (AttributeBase*)((uint8_t*)currentAttr + currentAttr->attributeLength);
    }

    return ret;
  }
};
const std::vector<const char*> MFTRecord::possibleMagicNumbers = {
  "FILE", // File record (the MFTRecord class actually implements this only for now) ( ntfsdoc-0.6/concepts/file_record.html )
  "BAAD", // "Unusable" entry
  "INDX" // Index record ( ntfsdoc-0.6/concepts/index_record.html )
}; // NOTE: there may be more, add as needed


enum MediaDescriptor: uint8_t {
  HardDisk = 0xF8,
  HighDensityFloppy = 0xF0
};

struct NTFS {
  // For more info see ntfsdoc-0.6/files/boot.html since this is $Boot. Also see https://www.cse.scu.edu/~tschwarz/coen252_07Fall/Lectures/NTFS.html where it says "Table 2: BPB and extended BPB fields on NTFS volumes".
  char x86JumpInstructionToTheBootLoaderRoutine[3]; // ntfsdoc-0.6/files/boot.html
  char systemID[8]; // "NTFS    "
  uint16_t bytesPerSector;
  uint8_t sectorsPerCluster;
  uint16_t reservedSectors; // "Reserved" value, "must be 0" (probably for forwards compatibility)
  char reserved0[3]; // "Reserved" value, "must be 0"
  char reserved1[2]; // "Reserved" value, "must be 0"
  MediaDescriptor mediaDescriptor;
  char reserved2[2]; // "Reserved" value, "must be 0"
  uint16_t sectorsPerTrack; // "Not used or checked by NTFS." according to the cse.scu.edu website but is something on ntfsdoc-0.6/files/boot.html
  uint16_t numberOfHeads; // "Not used or checked by NTFS." according to the cse.scu.edu website but is something on ntfsdoc-0.6/files/boot.html
  char notUsed0[2]; // "Not used or checked by NTFS."
  char notUsed1[2]; // "Not used or checked by NTFS."
  char notUsed2[4]; // "Not used or checked by NTFS."
  char reserved3[4]; // "Reserved" value, "must be 0"  // "Usually 80 00 80 00" + "A value of 80 00 00 00 has been seen on a USB thumb drive which is formatted with NTFS under Windows XP. Note this is removable media and is not partitioned, the drive as a whole is NTFS formatted." ( ntfsdoc-0.6/files/boot.html )
  uint64_t totalSectors; // "Number of sectors in the volume" ( ntfsdoc-0.6/files/boot.html )
  uint64_t mftOffset; // In clusters (LCNs)  // "LCN of VCN 0 of the $MFT" ( ntfsdoc-0.6/files/boot.html )
  uint64_t mftMirrOffset; // "Logical cluster number for the copy of the Master File Table (File $MFTmir)"  // "LCN of VCN 0 of the $MFTMirr" ( ntfsdoc-0.6/files/boot.html )
  uint32_t clustersPerMFTRecord; // "If the value is less than 7F, then this number is the clusters per Index Buffer. Otherwise, 2x, with x being the negative of this number, is the size of the file record." ( https://www.cse.scu.edu/~tschwarz/coen252_07Fall/Lectures/NTFS.html ) aka "This can be negative, which means that the size of the MFT/Index record is smaller than a cluster. In this case the size of the MFT/Index record in bytes is equal to 2^(-1 * Clusters per MFT/Index record). So for example if Clusters per MFT Record is 0xF6 (-10 in decimal), the MFT record size is 2^(-1 * -10) = 2^10 = 1024 bytes." ( ntfsdoc-0.6/files/boot.html )
  uint32_t clustersPerIndexRecord; // "Clusters per Index Buffer. If the value is less than 7F, then this number is the clusters per Index Buffer. Otherwise, 2x, with x being the negative of this number, is the size of the file record." ( https://www.cse.scu.edu/~tschwarz/coen252_07Fall/Lectures/NTFS.html ) aka "This can be negative, which means that the size of the MFT/Index record is smaller than a cluster. In this case the size of the MFT/Index record in bytes is equal to 2^(-1 * Clusters per MFT/Index record). So for example if Clusters per MFT Record is 0xF6 (-10 in decimal), the MFT record size is 2^(-1 * -10) = 2^10 = 1024 bytes." ( ntfsdoc-0.6/files/boot.html )
  uint64_t volumeSerialNumber;
  char notUsed4[4]; // "Not used or checked by NTFS."

  uint64_t bytesPerCluster() const {
    return bytesPerSector * sectorsPerCluster;
  }

  size_t mftOffsetInBytes() const {
    return mftOffset * bytesPerCluster();
  }

  // Reads in sizeof(MFTRecord) bytes from `fd` at the MFT starting location specified by this NTFS struct.
  MFTRecord getFirstMFTRecord(int fd) const {
    off_t offset = mftOffsetInBytes();
    _lseek(fd, offset, SEEK_SET);
    MFTRecord buf;
    _read(fd, &buf, sizeof(MFTRecord));
    return buf;
  }
};
static_assert(offsetof(NTFS, numberOfHeads) == 0x1A);
static_assert(offsetof(NTFS, totalSectors) == 0x28);
static_assert(offsetof(NTFS, mftMirrOffset) == 0x0038);
static_assert(offsetof(NTFS, notUsed4) == 0x50);

std::pair<MFTRecord* /*a pointer within the void* buffer*/,
	  unique_free<void*> /*the new buffer for use with `mdr`*/>
MFTRecord::next(const MyDataRuns& mdr, size_t amountAlreadyLoadedFromMDR, void* bufForMDR /*must be a malloc()'ed buffer*/,
     int fd, const NTFS* ntfs) const {
  // Read in a single MFTRecord by reading the number of clusters per MFT record.
  // FIXME: handle INDX for index records aka "index buffers" -- see NTFS struct and search for these terms for more info.
  bool moreNeeded; ssize_t more;
  auto ret = mdr.load(amountAlreadyLoadedFromMDR, bufForMDR, amountAlreadyLoadedFromMDR + ntfs->clustersPerMFTRecord * ntfs->bytesPerCluster(), fd, ntfs, &moreNeeded, &more);
  bufForMDR = ret.get();
  printf("MFTRecord::next: mdr.load set moreNeeded to %s and more to %jd\n", moreNeeded == true ? "true" : "false", (intmax_t)more);
  return std::make_pair((MFTRecord*)((uint8_t*)bufForMDR + amountAlreadyLoadedFromMDR + ntfs->clustersPerMFTRecord * ntfs->bytesPerCluster()), std::move(ret));
}

// Makes and loads a contiguous buffer from the dataRuns' specified offsets and lengths by dynamically allocating enough memory to hold it, then returning it. The buffer may be incomplete, i.e. if the amount available in `dataRuns` was less than `amountToLoad`. If so, `out_moreNeeded` will be set to true by this function.
// `bool out_moreNeeded` will be set to true if the `dataRuns` ran out before `amountToLoad` was reached; otherwise, it will be set to false.
// `out_more` will be set to a positive number indicating how much more was left to be loaded *if* `amountToLoad` was less than the total length of `dataRuns`. It will be set to a negative number if `amountToLoad` is greater than the total length of `dataRuns`. It will be set to zero otherwise.
// Returns a unique_ptr containing nullptr if `dataRuns.size() == 0`. If you provided the buffer, you may want to use .release() on the unique_ptr to take back ownership of the memory.
unique_free<void*> MyDataRuns::load(size_t bufOffset /*seek into the data runs by this amount before loading. Set to 0 for the first load. This number must be a multiple of `ntfs->bytesPerCluster()`.*/,
				    void* buf /*optional existing buffer. Set to nullptr to allocate a new one. If provided (non-null), this function will place more data only starting at buf + `bufOffset` provided.*/,
				    size_t amountToLoad,
				    int fd, const NTFS* ntfs, bool* out_moreNeeded, ssize_t* out_more) const {
  assert(bufOffset % ntfs->bytesPerCluster() == 0);
  
  _lseek(fd, 0, SEEK_SET); // Go to the start so we can use SEEK_CUR (to do a relative seek) later.
  
  size_t totalLength = 0;
  bool completeStruct = false;
  //void* buf = nullptr;
  // Assumptions (for now) //
  *out_more = 0;
  *out_moreNeeded = false;
  // //
  
  // NOTE: this is not checked because it is outside the scope/concerns of this function. This needs to be checked when using LazilyLoaded::loadUpTo().
  // if (dataRuns.hasMore) {
  //   *out_moreNeeded = true;
  // }
  
  for (MyDataRun dr /*copy so we can modify it without affecting the original*/ : dataRuns) {
    printf("MyDataRuns::load: processing: dr.offset = %ju, length = %ju\n", (uintmax_t)dr.offset, (uintmax_t)dr.length);
    if (dr.length == 0) {
      // completeStruct = true;
      // break;

      printf("MyDataRuns::load: dr.length == 0\n");
      continue;
    }
    
    size_t lengthToLoad = dr.length * ntfs->bytesPerCluster();
    printf("MyDataRuns::load: lengthToLoad: %zu bytes\n", lengthToLoad);
    
    // Seek further if needed
    if (dr.offset * ntfs->bytesPerCluster() + totalLength > bufOffset) {
      // We seeked as far as we need to, but need to modify the offset we start loading at within this run
      size_t newOffset = (dr.offset * ntfs->bytesPerCluster() + bufOffset) / ntfs->bytesPerCluster();
      printf("MyDataRuns::load: seeking within data run to get to bufOffset. Now at %ju bytes, was at %ju\n", (uintmax_t)(newOffset * ntfs->bytesPerCluster()), (uintmax_t)(dr.offset * ntfs->bytesPerCluster()));
      dr.offset = newOffset;
    }
    else {
      // We need to seek more (to the next run)
      totalLength += lengthToLoad;
      printf("MyDataRuns::load: seeking to the next data run to try to get past bufOffset. Now at %ju bytes\n", (uintmax_t)totalLength);
      continue;
    }
    
    if (totalLength + lengthToLoad > amountToLoad) { // TODO: untested
      // Limit length of what we load
      lengthToLoad = totalLength - lengthToLoad;
      printf("MyDataRuns::load: limiting length to %zu bytes\n", lengthToLoad);
      completeStruct = true;
      *out_more = lengthToLoad;
    }
    printf("MyDataRuns::load: calling realloc(%p, %zu) aka %f MiB\n", buf, totalLength+lengthToLoad, (float)(totalLength+lengthToLoad) / 1024 / 1024);
    buf = realloc(buf, totalLength+lengthToLoad);
    _lseek(fd, dr.offset * ntfs->bytesPerCluster(), SEEK_CUR); // Seek relative to the last seek
    _read(fd, (uint8_t*)buf+totalLength /*load into the position after where we wrote into `buf` last iteration*/ + bufOffset, lengthToLoad);
    _lseek(fd, -lengthToLoad, SEEK_CUR); // Seek back to the start of the run (since the fd's file offset was changed after the read())
    totalLength += lengthToLoad;
    if (totalLength >= amountToLoad || completeStruct) {
      // Done loading
      completeStruct = true;
      if (totalLength > amountToLoad) { // TODO: untested
	*out_more = amountToLoad - totalLength; // (Negative)
      }
      break;
    }
  }

  if (!completeStruct) { // Then `amountToLoad` was too large for the runs' contents, or the runs ran out because not enough was loaded.
    *out_moreNeeded = true; // TODO: untested
  }

  return unique_free<void*>((void**)buf);
}

std::pair<AttributeContentWithFreer, std::optional<MyDataRuns>> NonResidentAttribute::content(size_t limitToLoad, bool* out_moreNeeded, ssize_t* out_more, int fd, const NTFS* ntfs) const {
  // Load all the content virtually (since we can't load it all because it might be massive amounts of data)
  // Grab the runlist
  RunList* firstRunListEntry = (RunList*)((uint8_t*)this + offsetToTheRunList);
  // Grab its data runs
  size_t attrActualSize = 0; // 0 if unknown
  switch (base.typeIdentifier) {
  case STANDARD_INFORMATION:
    attrActualSize = sizeof(StandardInformation);
    break;
  case FILE_NAME:
    attrActualSize = sizeof(FileName);
    break;
  case DATA:
    break;
  default:
    throw UnhandledValue();
  }

  if (attrActualSize != 0) {
    if (attrActualSize > limitToLoad) {
      printf("NonResidentAttribute::content: warning: attrActualSize > limitToLoad. attrActualSize = %ju, limitToLoad = %ju. This means the whole structure won't be loaded in.\n", (uintmax_t)attrActualSize, (uintmax_t)limitToLoad);
    }
    limitToLoad = std::min(limitToLoad, attrActualSize);
  }
  MyDataRuns dr = LazilyLoaded{firstRunListEntry}.loadUpTo(limitToLoad);
  size_t bufOffset = 0; unique_free<void*> ptr = nullptr;
  ptr = dr.load(bufOffset, ptr.get(), limitToLoad, fd, ntfs, out_moreNeeded, out_more);
  printf("NonResidentAttribute::content: dr.load set out_moreNeeded to %s and out_more to %jd\n", *out_moreNeeded == true ? "true" : "false", (intmax_t)*out_more);
  switch (base.typeIdentifier) {
  case STANDARD_INFORMATION:
    return std::make_pair(AttributeContentWithFreer::make(unique_free<StandardInformation>((StandardInformation*)ptr.release())), std::make_optional(dr));
  case FILE_NAME:
    return std::make_pair(AttributeContentWithFreer::make(unique_free<FileName>((FileName*)ptr.release())), std::make_optional(dr));
  case DATA:
    return std::make_pair(AttributeContentWithFreer::make(unique_free<Data>((Data*)ptr.release())), std::make_optional(dr));
  default:
    throw UnhandledValue();
  }
}

// Returns the first attribute of type `attributeToFind` within `attributes`, or nullptr if not found.
template <typename AttributeContentT>
std::pair<TypedAttributeContentWithFreer<AttributeContentT>, std::optional<MyDataRuns>> findAttribute(const std::vector<Attribute>& attributes, AttributeTypeIdentifier attributeToFind, size_t limitToLoad /*max amount to load from a non-resident attribute*/, bool* out_moreNeeded /*for non-resident*/, ssize_t* out_more /*for non-resident*/, int fd, NTFS* ntfs) {
  uint8_t* attrAddress = nullptr;
  auto attrOfDesiredType = std::find_if(std::begin(attributes), std::end(attributes), [&attributeToFind, &attrAddress](auto attr){
    bool ret = false;
    auto process = [&](auto attr) -> bool {
      if (attr->base.typeIdentifier == attributeToFind) {
	attrAddress = (uint8_t*)attr;
	return true;
      }
      return false;
    };
    ret = std::visit([&](auto v){return process(v);}, attr);
    return ret;
  });
  if (attrOfDesiredType == std::end(attributes)) {
    return std::make_pair(TypedAttributeContentWithFreer<AttributeContentT>(), std::optional<MyDataRuns>()); // This attribute wasn't found
  }
  std::pair<TypedAttributeContentWithFreer<AttributeContentT>, std::optional<MyDataRuns>> content = std::visit([&](auto v){auto pair = v->content(limitToLoad, out_moreNeeded, out_more, fd, ntfs); return std::make_pair(TypedAttributeContentWithFreer<AttributeContentT>(std::move(pair.first)), std::move(pair.second));}, *attrOfDesiredType); // Get content()
  //AttributeContentT* desiredType = std::get<AttributeContentT*>(content); // Unwrap std::variant
  //return desiredType; // <-- can't do this because it returns free()'ed memory
  
  return content;
}

// "The second #pragma resets the pack value." ( https://stackoverflow.com/questions/24887459/c-c-struct-packing-not-working )
#pragma pack()

int main(int argc, char** argv) {
  int fd = _open(argv[1], O_RDONLY);

  struct NTFS buf;
  ssize_t ret = _read(fd, &buf, sizeof(NTFS));
  printf("mftOffset: %ju %ju\n", (uintmax_t)buf.mftOffset, (uintmax_t)(buf.mftOffset * buf.bytesPerCluster()));

  MFTRecord rec = buf.getFirstMFTRecord(fd);
  rec.applyFixup(buf.bytesPerSector);
  printf("numberOfThisMFTRecord: %ju , sequenceNumber: %ju ; fileReferenceAddress of first MFT record: computed %ju stored %ju\n",(uintmax_t)rec.numberOfThisMFTRecord, (uintmax_t)rec.sequenceNumber, (uintmax_t)rec.computedFileReferenceAddress(), (uintmax_t)rec.fileReferenceToTheBase_FILE_record);

  auto attributes = rec.attributes();
  for (auto& v : attributes) {
    // https://stackoverflow.com/questions/63482070/how-can-i-code-something-like-a-switch-for-stdvariant
    std::visit(overloaded{
	[](std::monostate&) /* Empty variant */ {},
	[](ResidentAttribute* v){BREAKPOINT;},
	[](NonResidentAttribute* v){BREAKPOINT;},
      }, v);
  }

  rec.hexDump();


  // Now that we have the first record, we know it is the $MFT itself (entry 0). So this is a file that references itself! We need to follow its $DATA attribute to get the full MFT contents. ( https://docs.microsoft.com/en-us/windows/win32/devnotes/master-file-table : "The $Mft file contains an unnamed $DATA attribute that is the sequence of MFT record segments, in order." )
  size_t limitToLoad = 1073741824; //max amount to load from a non-resident attribute
  bool moreNeeded; ssize_t more;
  auto file_name_pair = findAttribute<FileName>(attributes, FILE_NAME, limitToLoad, &moreNeeded, &more, fd, &buf);
  auto& file_name = file_name_pair.first;
  if (file_name.get() == nullptr) {
    printf("Can't find $FILE_NAME in first MFT entry.\n");
    return 1;
  }
  auto arr = file_name->fileNameInUnicode();
  auto str = arr.to_string();
  printf("Found $FILE_NAME in first MFT entry with file name: %s\n", str.c_str());

  auto data_pair = findAttribute<Data>(attributes, DATA, limitToLoad, &moreNeeded, &more, fd, &buf);
  auto& data = data_pair.first;
  if (data.get() == nullptr) {
    printf("Can't find $DATA in first MFT entry.\n");
    return 1;
  }
  printf("Found $DATA in first MFT entry\n");
  size_t limitToPrint = 2048, actualContentSize = limitToLoad+more;
  size_t amountToPrint = std::min(limitToPrint, actualContentSize);
  DumpHex(data.get(), amountToPrint);

  // Get $VOLUME record
  assert(data_pair.second.has_value()); // FIXME: if $MFT's $DATA attribute is resident this will fail. So unlikely but still a fixme.
  assert(data.isMalloced());
  auto recordAndBuf_mftMirr = rec.next(*data_pair.second, actualContentSize, data.get() /*this is also the buffer due to the above assertion*/, fd, &buf);

  _close(fd);
}
