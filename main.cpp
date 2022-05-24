#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include "ap/ap.hpp" // https://github.com/arbitrary-precision/ap
#include <cassert>

// Lib //
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

// https://www.cse.scu.edu/~tschwarz/coen252_07Fall/Lectures/NTFS.html

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
struct ArrayWithLength {
  T* array;
  size_t length;

  ArrayWithLengthIterator<T> begin() { return {array, length}; }
  ArrayWithLengthIterator<T> end() { return {array+length, 0}; }
};

// An entry within the MFT.
struct MFTRecord {
  char magicNumber[4]; // "FILE"
  uint16_t updateSequenceOffset;
  uint16_t numEntriesInFixupArray; // Fixup array = update sequence (synonymns).  // This is the number of entries where an entry is a single 16 bit value.
  uint64_t logFileSequenceNumber; // (LSN)  // "Each MFT record is addressed by a 48 bit MFT entry value [is simply the 0-based index of this record; an "entry index"].The first entry has address 0. Each MFT entry has a 16 bit sequence number that is incremented when the entry is allocated. MFT entry value and sequence number combined yield 64b [bit] file reference address."
  uint16_t sequenceNumber;
  uint16_t hardLinkCount; // "The hard link count is the number of directory entries that reference this record."
  uint16_t offsetToFirstAttribute; // *useful*
  MFTEntryFlags flags;
  uint32_t usedSizeOfMFTEntry;
  uint32_t allocatedSizeOfMFTEntry;
  uint64_t fileReferenceToTheBase_FILE_record; // *?*
  uint16_t nextAttributeID; // *?*
  char padding10[2]; // "Align to 4B boundary" on Windows XP
  // [I think this is this but not sure:] The "entry value" or "entry number" for this MFTRecord. This is just the 0-based index of this record basically.
  uint32_t numberOfThisMFTRecord; // On Windows XP
  char attributesAndFixupValue[0x1000-0x30]; // Attributes and fixup value

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

  _close(fd);
}
