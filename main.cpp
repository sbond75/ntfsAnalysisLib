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
  off_t ret = lseek(fd, offset, whence);
  if (ret == -1) {
    perror("lseek failed");
    throw errno;
  }
  else if (ret != offset) {
    perror("lseek didn't go to the expected offset");
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
  RecordInUse = 0x01,
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
    
  T& operator* ()
  {
    if (entriesRemainingIncludingThisOne == 0) {
      printf("No more entries left in ArrayWithLengthIterator");
      throw entriesRemainingIncludingThisOne;
    }
    return *val;
  }

  T* operator-> ()
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
  uint16_t attributeIdentifier; // "Each attribute has a unique identifier" ( ntfsdoc-0.6/concepts/attribute_header.html )
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

// Resident = in this MFT. These have a different structure from non-resident ones.
struct ResidentAttribute {
  AttributeBase base;
  uint32_t sizeOfContent;
  uint16_t offsetToContent;
  uint8_t indexedFlag; // ntfsdoc-0.6/concepts/attribute_header.html
  char padding[1]; // ntfsdoc-0.6/concepts/attribute_header.html

  // Returns the attribute name or nullptr if this is not a named attribute.
  ArrayWithLength<uint16_t> name() {
    if (base.lengthOfName != 0) {
      assert(base.offsetToName != 0);
      return {(uint16_t*)((uint8_t*)this + base.offsetToName), base.lengthOfName};
    }
    else {
      //assert(base.offsetToName == 0);
      return {nullptr, 0};
    }
  }

  AttributeContent content() {
    uint8_t* contentPtr = (uint8_t*)this + offsetToContent;
    switch (base.typeIdentifier) {
    case STANDARD_INFORMATION:
      return (StandardInformation*)contentPtr;
    case FILE_NAME:
      return (FileName*)contentPtr;
    default:
      throw UnhandledValue();
    }
  }
};

// "non-resident attributes need to describe an arbitrary number of cluster runs, consecutive clusters that they occupy."
struct NonResidentAttribute {
  AttributeBase base;
  uint64_t startingVirtualClusterNumber;
  uint64_t endingVirtualClusterNumber;
  uint16_t offsetToTheRunList;
  uint16_t compressionUnitSize;
  uint32_t unused;
  uint64_t allocatedSizeOfTheAttributeContent;
  uint64_t actualSizeOfTheAttributeContent;
  uint64_t initializedSizeOfTheAttributeContent;
  
  AttributeContent content() {
    throw UnhandledValue(); // Not yet implemented actually
  }
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
  uint64_t logFileSequenceNumber; // (LSN)  // "Each MFT record is addressed by a 48 bit MFT entry value [is simply the 0-based index of this record; an "entry index"].The first entry has address 0. Each MFT entry has a 16 bit sequence number that is incremented when the entry is allocated. MFT entry value and sequence number combined yield 64b [bit] file reference address."
  uint16_t sequenceNumber; // Says how many times this entry has been used.
  uint16_t hardLinkCount; // "The hard link count is the number of directory entries that reference this record."
  uint16_t offsetToFirstAttribute; // *useful*
  MFTEntryFlags flags;
  uint32_t usedSizeOfMFTEntry;
  uint32_t allocatedSizeOfMFTEntry;
  uint64_t fileReferenceToTheBase_FILE_record; // "MFT entries could be larger than fit into the normal space. In this case, the MFT entry will start in the base MFT record and continued in an extension record." If the file reference to the base file entry is 0x 00 00 00 00 00 00 00 00 then this is a base record. Were it not so, then this field would contain a reference to the base MFT record.
  uint16_t nextAttributeID; // This is the "next attribute ID" in the sense that it is the next attribute ID to place into this MFTRecord *if* you are adding a new attribute entry I think. Since the attributes are in ascending order by ID apparently. Anyway, main point is that numAttributes() is based on this.
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
    printf("  \tSize: %ju\n", size);
  }

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

    printf("numAttributes: counter %ju, retval %ju\n", counter, retval);
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
      printf("applyFixup: %ju should be usn %ju", *valPtr, usn);
      fflush(stdout);
      assert(*valPtr == usn);
      printf("; %ju -> %ju\n", *valPtr, val);
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
      currentAttr = (AttributeBase*)((uint8_t*)currentAttr + currentAttr->attributeLength);
    }

    return ret;
  }
};

struct NTFS {
  char padding0[0x0B];
  uint16_t bytesPerSector;
  char padding10[0x0D-0x0B-sizeof(uint16_t)];
  uint8_t sectorsPerCluster;
  char padding20[0x30-0x0D-sizeof(uint8_t)];
  uint64_t mftOffset; // In clusters

  uint64_t bytesPerCluster() const {
    return bytesPerSector * sectorsPerCluster;
  }

  MFTRecord getFirstMFTRecord(int fd) const {
    off_t offset = mftOffset * bytesPerCluster();
    _lseek(fd, offset, SEEK_SET);
    MFTRecord buf;
    _read(fd, &buf, sizeof(MFTRecord));
    return buf;
  }
};

// Returns the first attribute of type `attributeToFind` within `attributes`, or nullptr if not found.
template <typename AttributeContentT>
AttributeContentT* findAttribute(const std::vector<Attribute>& attributes, AttributeTypeIdentifier attributeToFind) {
  auto attrOfDesiredType = std::find_if(std::begin(attributes), std::end(attributes), [&attributeToFind](auto attr){
    bool ret = false;
    auto process = [&](auto attr) -> bool {
      return (attr->base.typeIdentifier == attributeToFind);
    };
    ret = std::visit([&](auto v){return process(v);}, attr);
    return ret;
  });
  if (attrOfDesiredType == std::end(attributes)) {
    return nullptr;
  }
  AttributeContent content = std::visit([](auto v){return v->content();}, *attrOfDesiredType); // Get content()
  AttributeContentT* desiredType = std::get<AttributeContentT*>(content); // Unwrap std::variant
  return desiredType;
}

// "The second #pragma resets the pack value." ( https://stackoverflow.com/questions/24887459/c-c-struct-packing-not-working )
#pragma pack()

int main(int argc, char** argv) {
  int fd = _open(argv[1], O_RDONLY);

  struct NTFS buf;
  ssize_t ret = _read(fd, &buf, sizeof(NTFS));
  printf("mftOffset: %ju %ju\n", buf.mftOffset, buf.mftOffset * buf.bytesPerCluster());

  MFTRecord rec = buf.getFirstMFTRecord(fd);
  rec.applyFixup(buf.bytesPerSector);
  printf("numberOfThisMFTRecord: %ju , sequenceNumber: %ju ; fileReferenceAddress of first MFT record: computed %ju stored %ju\n",rec.numberOfThisMFTRecord, rec.sequenceNumber, rec.computedFileReferenceAddress(), rec.fileReferenceToTheBase_FILE_record);

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
  auto file_name = findAttribute<FileName>(attributes, FILE_NAME);
  if (file_name == nullptr) {
    printf("Can't find $FILE_NAME in first MFT entry.\n");
    return 1;
  }
  auto arr = file_name->fileNameInUnicode();
  auto str = arr.to_string();
  printf("Found $FILE_NAME in first MFT entry (resident) with file name: %s\n", str.c_str());

  auto data = findAttribute<Data>(attributes, DATA);
  if (data == nullptr) {
    printf("Can't find $DATA in first MFT entry.\n");
    return 1;
  }
  printf("Found $DATA in first MFT entry\n");

  _close(fd);
}
