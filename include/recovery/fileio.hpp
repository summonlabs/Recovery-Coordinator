#ifndef RECOVERY_FILEIO_HPP
#define RECOVERY_FILEIO_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "recovery/error.hpp"

namespace recovery {

// Small, explicit file surface used by the durability layer.
//
// Everything here is blocking and synchronous on purpose: the journal makes a
// durability claim, and a durability claim that is not backed by a completed
// device flush is not a claim the coordinator is allowed to make.
namespace fileio {

// CRC-32C (Castagnoli). Used for intra-record integrity so that a torn or
// damaged byte range is detected before it is interpreted.
[[nodiscard]] std::uint32_t crc32c(const void* data, std::size_t size) noexcept;
[[nodiscard]] std::uint32_t crc32c_extend(std::uint32_t seed, const void* data, std::size_t size) noexcept;

[[nodiscard]] bool exists(const std::string& path);
[[nodiscard]] std::optional<std::uint64_t> file_size(const std::string& path);

// Reads a whole file. Refuses to read more than max_bytes, so an absurd or
// hostile file cannot exhaust memory.
[[nodiscard]] Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::uint64_t max_bytes);

// Writes buffer to a staging file, flushes it to the device, reads it back,
// compares it byte for byte, and only then replaces the destination
// atomically. Returns a persistence error if any step fails; the destination
// is left untouched in that case.
[[nodiscard]] Result<bool> publish_atomically(const std::string& path, const std::vector<std::uint8_t>& content);

// Removes a file. A missing file is not an error.
[[nodiscard]] Result<bool> remove_file(const std::string& path);

// Truncates a file to a byte length. Used only to cut a torn tail, which is a
// recovery action the caller must have decided on explicitly.
[[nodiscard]] Result<bool> truncate_file(const std::string& path, std::uint64_t length);

// Creates every missing parent directory of a path.
[[nodiscard]] Result<bool> ensure_parent_directory(const std::string& path);

// Absolute, lexically normalised path. Used for diagnostics and for tests that
// must prove two processes are addressing the same file.
[[nodiscard]] std::string absolute_path(const std::string& path);

}  // namespace fileio
}  // namespace recovery

#endif  // RECOVERY_FILEIO_HPP
