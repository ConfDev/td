//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2018
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/utils/port/FileFd.h"

#if TD_PORT_WINDOWS
#include "td/utils/misc.h"  // for narrow_cast

#include "td/utils/port/Stat.h"
#include "td/utils/port/wstring_convert.h"
#endif

#include "td/utils/logging.h"
#include "td/utils/misc.h"
#include "td/utils/port/sleep.h"
#include "td/utils/StringBuilder.h"

#include "td/utils/port/detail/PollableFd.h"

#include <cstring>

#if TD_PORT_POSIX
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace td {

namespace {

struct PrintFlags {
  int32 flags;
};

StringBuilder &operator<<(StringBuilder &sb, const PrintFlags &print_flags) {
  auto flags = print_flags.flags;
  if (flags &
      ~(FileFd::Write | FileFd::Read | FileFd::Truncate | FileFd::Create | FileFd::Append | FileFd::CreateNew)) {
    return sb << "opened with invalid flags " << flags;
  }

  if (flags & FileFd::Create) {
    sb << "opened/created ";
  } else if (flags & FileFd::CreateNew) {
    sb << "created ";
  } else {
    sb << "opened ";
  }

  if ((flags & FileFd::Write) && (flags & FileFd::Read)) {
    if (flags & FileFd::Append) {
      sb << "for reading and appending";
    } else {
      sb << "for reading and writing";
    }
  } else if (flags & FileFd::Write) {
    if (flags & FileFd::Append) {
      sb << "for appending";
    } else {
      sb << "for writing";
    }
  } else if (flags & FileFd::Read) {
    sb << "for reading";
  } else {
    sb << "for nothing";
  }

  if (flags & FileFd::Truncate) {
    sb << " with truncation";
  }
  return sb;
}

}  // namespace

namespace detail {
class FileFdImpl {
 public:
  PollableFdInfo info;
};
}  // namespace detail

FileFd::FileFd() = default;
FileFd::FileFd(FileFd &&) = default;
FileFd &FileFd::operator=(FileFd &&) = default;
FileFd::~FileFd() = default;

FileFd::FileFd(std::unique_ptr<detail::FileFdImpl> impl) : impl_(std::move(impl)) {
}

Result<FileFd> FileFd::open(CSlice filepath, int32 flags, int32 mode) {
  if (flags & ~(Write | Read | Truncate | Create | Append | CreateNew)) {
    return Status::Error(PSLICE() << "File \"" << filepath << "\" has failed to be " << PrintFlags{flags});
  }

  if ((flags & (Write | Read)) == 0) {
    return Status::Error(PSLICE() << "File \"" << filepath << "\" can't be " << PrintFlags{flags});
  }

#if TD_PORT_POSIX
  int native_flags = 0;

  if ((flags & Write) && (flags & Read)) {
    native_flags |= O_RDWR;
  } else if (flags & Write) {
    native_flags |= O_WRONLY;
  } else {
    CHECK(flags & Read);
    native_flags |= O_RDONLY;
  }

  if (flags & Truncate) {
    native_flags |= O_TRUNC;
  }

  if (flags & Create) {
    native_flags |= O_CREAT;
  } else if (flags & CreateNew) {
    native_flags |= O_CREAT;
    native_flags |= O_EXCL;
  }

  if (flags & Append) {
    native_flags |= O_APPEND;
  }

  int native_fd = skip_eintr([&] { return ::open(filepath.c_str(), native_flags, static_cast<mode_t>(mode)); });
  if (native_fd < 0) {
    return OS_ERROR(PSLICE() << "File \"" << filepath << "\" can't be " << PrintFlags{flags});
  }

  return from_native_fd(NativeFd(native_fd));
#elif TD_PORT_WINDOWS
  // TODO: support modes
  auto r_filepath = to_wstring(filepath);
  if (r_filepath.is_error()) {
    return Status::Error(PSLICE() << "Failed to convert file path \" << filepath << \" to UTF-16");
  }
  auto w_filepath = r_filepath.move_as_ok();
  DWORD desired_access = 0;
  if ((flags & Write) && (flags & Read)) {
    desired_access |= GENERIC_READ | GENERIC_WRITE;
  } else if (flags & Write) {
    desired_access |= GENERIC_WRITE;
  } else {
    CHECK(flags & Read);
    desired_access |= GENERIC_READ;
  }

  // TODO: share mode
  DWORD share_mode = FILE_SHARE_READ | FILE_SHARE_DELETE | FILE_SHARE_WRITE;

  DWORD creation_disposition = 0;
  if (flags & Create) {
    if (flags & Truncate) {
      creation_disposition = CREATE_ALWAYS;
    } else {
      creation_disposition = OPEN_ALWAYS;
    }
  } else if (flags & CreateNew) {
    creation_disposition = CREATE_NEW;
  } else {
    if (flags & Truncate) {
      creation_disposition = TRUNCATE_EXISTING;
    } else {
      creation_disposition = OPEN_EXISTING;
    }
  }

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_SYSTEM)
  auto handle = CreateFile(w_filepath.c_str(), desired_access, share_mode, nullptr, creation_disposition, 0, nullptr);
#else
  auto handle = CreateFile2(w_filepath.c_str(), desired_access, share_mode, creation_disposition, nullptr);
#endif
  if (handle == INVALID_HANDLE_VALUE) {
    return OS_ERROR(PSLICE() << "File \"" << filepath << "\" can't be " << PrintFlags{flags});
  }
  auto native_fd = NativeFd(handle);
  if (flags & Append) {
    LARGE_INTEGER offset;
    offset.QuadPart = 0;
    auto set_pointer_res = SetFilePointerEx(handle, offset, nullptr, FILE_END);
    if (!set_pointer_res) {
      auto res = OS_ERROR(PSLICE() << "Failed to seek to the end of file \"" << filepath << "\"");
      return res;
    }
  }
  return from_native_fd(std::move(native_fd));
#endif
}

Result<FileFd> FileFd::from_native_fd(NativeFd native_fd) {
  auto impl = std::make_unique<detail::FileFdImpl>();
  impl->info.set_native_fd(std::move(native_fd));
  impl->info.add_flags(PollFlags::Write());
  return FileFd(std::move(impl));
}

Result<size_t> FileFd::write(Slice slice) {
#if TD_PORT_POSIX
  auto native_fd = get_native_fd().fd();
  auto write_res = skip_eintr([&] { return ::write(native_fd, slice.begin(), slice.size()); });
  if (write_res >= 0) {
    return narrow_cast<size_t>(write_res);
  }

  auto write_errno = errno;
  auto error = Status::PosixError(write_errno, PSLICE() << "Write to [fd = " << native_fd << "] has failed");
  if (write_errno != EAGAIN
#if EAGAIN != EWOULDBLOCK
      && write_errno != EWOULDBLOCK
#endif
      && write_errno != EIO) {
    LOG(ERROR) << error;
  }
  return std::move(error);
#elif TD_PORT_WINDOWS
  auto native_fd = get_native_fd().io_handle();
  DWORD bytes_written = 0;
  auto res = WriteFile(native_fd, slice.data(), narrow_cast<DWORD>(slice.size()), &bytes_written, nullptr);
  if (!res) {
    return OS_ERROR("Failed to write_sync");
  }
  return bytes_written;
#endif
}

Result<size_t> FileFd::read(MutableSlice slice) {
#if TD_PORT_POSIX
  auto native_fd = get_native_fd().fd();
  auto read_res = skip_eintr([&] { return ::read(native_fd, slice.begin(), slice.size()); });
  auto read_errno = errno;

  if (read_res >= 0) {
    if (narrow_cast<size_t>(read_res) < slice.size()) {
      get_poll_info().clear_flags(PollFlags::Read());
    }
    return static_cast<size_t>(read_res);
  }

  auto error = Status::PosixError(read_errno, PSLICE() << "Read from [fd = " << native_fd << "] has failed");
  if (read_errno != EAGAIN
#if EAGAIN != EWOULDBLOCK
      && read_errno != EWOULDBLOCK
#endif
      && read_errno != EIO) {
    LOG(ERROR) << error;
  }
  return std::move(error);
#elif TD_PORT_WINDOWS
  auto native_fd = get_native_fd().io_handle();
  DWORD bytes_read = 0;
  auto res = ReadFile(native_fd, slice.data(), narrow_cast<DWORD>(slice.size()), &bytes_read, nullptr);
  if (!res) {
    return OS_ERROR("Failed to read_sync");
  }
  if (bytes_read == 0) {
    get_poll_info().clear_flags(PollFlags::Read());
  }
  return bytes_read;
#endif
}

Result<size_t> FileFd::pwrite(Slice slice, int64 offset) {
  if (offset < 0) {
    return Status::Error("Offset must be non-negative");
  }
#if TD_PORT_POSIX
  auto native_fd = get_native_fd().fd();
  TRY_RESULT(offset_off_t, narrow_cast_safe<off_t>(offset));
  auto pwrite_res = skip_eintr([&] { return ::pwrite(native_fd, slice.begin(), slice.size(), offset_off_t); });
  if (pwrite_res >= 0) {
    return narrow_cast<size_t>(pwrite_res);
  }

  auto pwrite_errno = errno;
  auto error = Status::PosixError(
      pwrite_errno, PSLICE() << "Pwrite to [fd = " << native_fd << "] at [offset = " << offset << "] has failed");
  if (pwrite_errno != EAGAIN
#if EAGAIN != EWOULDBLOCK
      && pwrite_errno != EWOULDBLOCK
#endif
      && pwrite_errno != EIO) {
    LOG(ERROR) << error;
  }
  return std::move(error);
#elif TD_PORT_WINDOWS
  auto native_fd = get_native_fd().io_handle();
  DWORD bytes_written = 0;
  OVERLAPPED overlapped;
  std::memset(&overlapped, 0, sizeof(overlapped));
  overlapped.Offset = static_cast<DWORD>(offset);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  auto res = WriteFile(native_fd, slice.data(), narrow_cast<DWORD>(slice.size()), &bytes_written, &overlapped);
  if (!res) {
    return OS_ERROR("Failed to pwrite");
  }
  return bytes_written;
#endif
}

Result<size_t> FileFd::pread(MutableSlice slice, int64 offset) {
  if (offset < 0) {
    return Status::Error("Offset must be non-negative");
  }
#if TD_PORT_POSIX
  auto native_fd = get_native_fd().fd();
  TRY_RESULT(offset_off_t, narrow_cast_safe<off_t>(offset));
  auto pread_res = skip_eintr([&] { return ::pread(native_fd, slice.begin(), slice.size(), offset_off_t); });
  if (pread_res >= 0) {
    return narrow_cast<size_t>(pread_res);
  }

  auto pread_errno = errno;
  auto error = Status::PosixError(
      pread_errno, PSLICE() << "Pread from [fd = " << native_fd << "] at [offset = " << offset << "] has failed");
  if (pread_errno != EAGAIN
#if EAGAIN != EWOULDBLOCK
      && pread_errno != EWOULDBLOCK
#endif
      && pread_errno != EIO) {
    LOG(ERROR) << error;
  }
  return std::move(error);
#elif TD_PORT_WINDOWS
  auto native_fd = get_native_fd().io_handle();
  DWORD bytes_read = 0;
  OVERLAPPED overlapped;
  std::memset(&overlapped, 0, sizeof(overlapped));
  overlapped.Offset = static_cast<DWORD>(offset);
  overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
  auto res = ReadFile(native_fd, slice.data(), narrow_cast<DWORD>(slice.size()), &bytes_read, &overlapped);
  if (!res) {
    return OS_ERROR("Failed to pread");
  }
  return bytes_read;
#endif
}

Status FileFd::lock(FileFd::LockFlags flags, int32 max_tries) {
  if (max_tries <= 0) {
    return Status::Error(0, "Can't lock file: wrong max_tries");
  }
#if TD_PORT_POSIX
  auto native_fd = get_native_fd().fd();
#elif TD_PORT_WINDOWS
  auto native_fd = get_native_fd().io_handle();
#endif
  while (true) {
#if TD_PORT_POSIX
    struct flock lock;
    std::memset(&lock, 0, sizeof(lock));

    lock.l_type = static_cast<short>([&] {
      switch (flags) {
        case LockFlags::Read:
          return F_RDLCK;
        case LockFlags::Write:
          return F_WRLCK;
        case LockFlags::Unlock:
          return F_UNLCK;
        default:
          UNREACHABLE();
          return F_UNLCK;
      }
    }());

    lock.l_whence = SEEK_SET;
    if (fcntl(native_fd, F_SETLK, &lock) == -1) {
      if (errno == EAGAIN) {
#elif TD_PORT_WINDOWS
    OVERLAPPED overlapped;
    std::memset(&overlapped, 0, sizeof(overlapped));

    BOOL result;
    if (flags == LockFlags::Unlock) {
      result = UnlockFileEx(native_fd, 0, MAXDWORD, MAXDWORD, &overlapped);
    } else {
      DWORD dw_flags = LOCKFILE_FAIL_IMMEDIATELY;
      if (flags == LockFlags::Write) {
        dw_flags |= LOCKFILE_EXCLUSIVE_LOCK;
      }

      result = LockFileEx(native_fd, dw_flags, 0, MAXDWORD, MAXDWORD, &overlapped);
    }

    if (!result) {
      if (GetLastError() == ERROR_LOCK_VIOLATION) {
#endif
        if (--max_tries > 0) {
          usleep_for(100000);
          continue;
        }

        return OS_ERROR("Can't lock file because it is already in use; check for another program instance running");
      }

      return OS_ERROR("Can't lock file");
    }
    return Status::OK();
  }
}

void FileFd::close() {
  impl_.reset();
}

bool FileFd::empty() const {
  return !impl_;
}

const NativeFd &FileFd::get_native_fd() const {
  return get_poll_info().native_fd();
}

NativeFd FileFd::move_as_native_fd() {
  auto res = get_poll_info().move_as_native_fd();
  impl_.reset();
  return res;
}

int64 FileFd::get_size() {
  return stat().size_;
}

#if TD_PORT_WINDOWS
static uint64 filetime_to_unix_time_nsec(LONGLONG filetime) {
  const auto FILETIME_UNIX_TIME_DIFF = 116444736000000000ll;
  return static_cast<uint64>((filetime - FILETIME_UNIX_TIME_DIFF) * 100);
}
#endif

Stat FileFd::stat() {
  CHECK(!empty());
#if TD_PORT_POSIX
  return detail::fstat(get_native_fd().fd());
#elif TD_PORT_WINDOWS
  Stat res;

  FILE_BASIC_INFO basic_info;
  auto status =
      GetFileInformationByHandleEx(get_native_fd().io_handle(), FileBasicInfo, &basic_info, sizeof(basic_info));
  if (!status) {
    auto error = OS_ERROR("Stat failed");
    LOG(FATAL) << error;
  }
  res.atime_nsec_ = filetime_to_unix_time_nsec(basic_info.LastAccessTime.QuadPart);
  res.mtime_nsec_ = filetime_to_unix_time_nsec(basic_info.LastWriteTime.QuadPart);
  res.is_dir_ = (basic_info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  res.is_reg_ = true;

  FILE_STANDARD_INFO standard_info;
  status = GetFileInformationByHandleEx(get_native_fd().io_handle(), FileStandardInfo, &standard_info,
                                        sizeof(standard_info));
  if (!status) {
    auto error = OS_ERROR("Stat failed");
    LOG(FATAL) << error;
  }
  res.size_ = standard_info.EndOfFile.QuadPart;

  return res;
#endif
}

Status FileFd::sync() {
  CHECK(!empty());
#if TD_PORT_POSIX
  if (fsync(get_native_fd().fd()) != 0) {
#elif TD_PORT_WINDOWS
  if (FlushFileBuffers(get_native_fd().io_handle()) == 0) {
#endif
    return OS_ERROR("Sync failed");
  }
  return Status::OK();
}

Status FileFd::seek(int64 position) {
  CHECK(!empty());
#if TD_PORT_POSIX
  TRY_RESULT(position_off_t, narrow_cast_safe<off_t>(position));
  if (skip_eintr([&] { return ::lseek(get_native_fd().fd(), position_off_t, SEEK_SET); }) < 0) {
#elif TD_PORT_WINDOWS
  LARGE_INTEGER offset;
  offset.QuadPart = position;
  if (SetFilePointerEx(get_native_fd().io_handle(), offset, nullptr, FILE_BEGIN) == 0) {
#endif
    return OS_ERROR("Seek failed");
  }
  return Status::OK();
}

Status FileFd::truncate_to_current_position(int64 current_position) {
  CHECK(!empty());
#if TD_PORT_POSIX
  TRY_RESULT(current_position_off_t, narrow_cast_safe<off_t>(current_position));
  if (skip_eintr([&] { return ::ftruncate(get_native_fd().fd(), current_position_off_t); }) < 0) {
#elif TD_PORT_WINDOWS
  if (SetEndOfFile(get_native_fd().io_handle()) == 0) {
#endif
    return OS_ERROR("Truncate failed");
  }
  return Status::OK();
}
PollableFdInfo &FileFd::get_poll_info() {
  return impl_->info;
}
const PollableFdInfo &FileFd::get_poll_info() const {
  return impl_->info;
}

}  // namespace td
