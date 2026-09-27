// rmv-report: summarize a Radeon Memory Visualizer (.rmv) trace on the
// command line.
//
// Reading an .rmv file is pure file processing: no AMD GPU, no AMD driver and
// no Qt are involved. This tool links against the parser and backend of AMD's
// Radeon Memory Visualizer (which do all the format decoding) and prints a
// text report instead of opening the GUI.
//
// Both trace formats in circulation are handled by AMD's parser:
//   "MINI"      written by Mesa's RADV driver (MESA_VK_TRACE=rmv)
//   "AMD_RDF "  written by the Radeon Developer Panel (RDF container, zstd)
//
// The report samples a number of evenly spaced points in time. It first prints
// one summary row per point, so values can be followed over time at a glance,
// and then a detailed block per point: heap status, usage breakdown, where the
// memory is actually backed, the allocation size distribution in
// power-of-two classes, and the most frequent resource sizes.

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

#include "rmt_data_set.h"
#include "rmt_data_snapshot.h"
#include "rmt_error.h"
#include "rmt_format.h"
#include "rmt_print.h"
#include "rmt_trace_loader.h"
#include "rmt_types.h"
#include "rmt_virtual_allocation_list.h"

namespace
{
const int kDefaultPoints   = 10;
const int kMaxPoints       = 1000;
const int kUsageRows       = 6;   // usage types listed per point
const int kTopSizeRows     = 8;   // resource sizes listed per point
const int kBarWidth        = 30;  // width of the '#' bars

const RmtHeapType kHeaps[] = {kRmtHeapTypeLocal, kRmtHeapTypeInvisible, kRmtHeapTypeSystem};

// ---------------------------------------------------------------- formatting

const double kKiB = 1024.0;
const double kMiB = 1024.0 * 1024.0;
const double kGiB = 1024.0 * 1024.0 * 1024.0;

double ToGiB(uint64_t bytes)
{
    return static_cast<double>(bytes) / kGiB;
}

double ToMiB(uint64_t bytes)
{
    return static_cast<double>(bytes) / kMiB;
}

// Human-readable size with an automatically chosen unit. Sizes below 1 KiB
// are shown in bytes; rounding them to "0 KiB" would make different sizes
// look identical.
std::string Size(uint64_t bytes)
{
    char buffer[32];
    const double b = static_cast<double>(bytes);
    if (bytes < 1024)
        snprintf(buffer, sizeof(buffer), "%" PRIu64 " B", bytes);
    else if (b < kMiB)
        snprintf(buffer, sizeof(buffer), (bytes % 1024 == 0) ? "%.0f KiB" : "%.1f KiB", b / kKiB);
    else if (b < kGiB)
        snprintf(buffer, sizeof(buffer), "%.2f MiB", b / kMiB);
    else
        snprintf(buffer, sizeof(buffer), "%.2f GiB", b / kGiB);
    return buffer;
}

// Integer with thousands separators, e.g. 27,657,548,458.
std::string Grouped(uint64_t value)
{
    std::string digits = std::to_string(value);
    std::string out;
    int         count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it)
    {
        if (count > 0 && count % 3 == 0)
            out.insert(out.begin(), ',');
        out.insert(out.begin(), *it);
        ++count;
    }
    return out;
}

std::string Bar(double fraction)
{
    int n = static_cast<int>(fraction * kBarWidth + 0.5);
    if (fraction > 0.0 && n == 0)
        n = 1;  // make non-zero shares visible
    return std::string(static_cast<size_t>(std::min(n, kBarWidth)), '#');
}

double Share(uint64_t part, uint64_t whole)
{
    return whole ? static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

void Heading(const char* title)
{
    printf("\n%s\n", title);
    printf("%s\n", std::string(strlen(title), '=').c_str());
}

const char* HeapName(RmtHeapType heap)
{
    switch (heap)
    {
    case kRmtHeapTypeLocal:     return "Local";
    case kRmtHeapTypeInvisible: return "Invisible";
    case kRmtHeapTypeSystem:    return "System";
    default:                    return "?";
    }
}

const char* HeapDescription(RmtHeapType heap)
{
    switch (heap)
    {
    case kRmtHeapTypeLocal:     return "VRAM, CPU-visible";
    case kRmtHeapTypeInvisible: return "VRAM, not CPU-visible";
    case kRmtHeapTypeSystem:    return "host memory";
    default:                    return "";
    }
}

const char* UsageName(int usage)
{
    return RmtGetResourceUsageTypeNameFromResourceUsageType(static_cast<RmtResourceUsageType>(usage));
}

// ------------------------------------------------------------- measurements

// Everything the report prints for one point in time. It is collected first
// and printed afterwards, so that the summary table can come before the
// detailed blocks.
struct PointReport
{
    int      percent   = 0;
    uint64_t timestamp = 0;

    RmtSegmentStatus heap_status[3] = {};  // indexed like kHeaps
    bool             heap_valid[3]  = {};

    std::vector<std::pair<uint64_t, int>> local_by_usage;  // (bytes, usage), sorted descending
    uint64_t                              local_usage_total = 0;

    uint64_t backed[kRmtHeapTypeCount + 1] = {};  // last entry: unbacked

    int      allocation_count   = 0;
    double   mean_fragmentation = 0.0;
    uint64_t largest_resource   = 0;

    uint64_t             size_median = 0, size_p90 = 0, size_p99 = 0, size_total = 0;
    std::map<int, int>   per_class;  // log2(size, rounded up) -> count

    // (count, size) sorted descending, plus the usage breakdown per size
    std::vector<std::pair<int, uint64_t>>  top_sizes;
    std::map<uint64_t, std::map<int, int>> usage_per_size;
};

bool Measure(RmtDataSet* data_set, int index, int points, PointReport* out)
{
    out->timestamp = (data_set->maximum_timestamp * index) / points;
    out->percent   = (index * 100) / points;

    char name[64];
    snprintf(name, sizeof(name), "p%02d", index);

    RmtSnapshotPoint* snapshot_point = nullptr;
    if (RmtDataSetAddSnapshot(data_set, name, out->timestamp, &snapshot_point) != kRmtOk || snapshot_point == nullptr)
    {
        fprintf(stderr, "warning: point %d could not be created\n", index);
        return false;
    }

    // Do NOT move this to the stack. RmtDataSnapshot is several megabytes
    // large; as a local variable it overflows the default stack before the
    // first statement of the function runs.
    std::vector<uint8_t> snapshot_storage(sizeof(RmtDataSnapshot), 0);
    RmtDataSnapshot*     snapshot = reinterpret_cast<RmtDataSnapshot*>(snapshot_storage.data());
    if (RmtDataSetGenerateSnapshot(data_set, snapshot_point, snapshot) != kRmtOk)
    {
        fprintf(stderr, "warning: snapshot for point %d failed\n", index);
        return false;
    }

    // Per-heap status.
    for (int h = 0; h < 3; ++h)
    {
        memset(&out->heap_status[h], 0, sizeof(RmtSegmentStatus));
        out->heap_valid[h] = RmtDataSnapshotGetSegmentStatus(snapshot, kHeaps[h], &out->heap_status[h]) == kRmtOk;
    }

    // Usage breakdown of the local heap: which kind of resource dominates VRAM.
    if (out->heap_valid[0])
    {
        const RmtSegmentStatus& local = out->heap_status[0];
        for (int u = 0; u < kRmtResourceUsageTypeCount; ++u)
        {
            if (local.physical_bytes_per_resource_usage[u] > 0)
            {
                out->local_by_usage.emplace_back(local.physical_bytes_per_resource_usage[u], u);
                out->local_usage_total += local.physical_bytes_per_resource_usage[u];
            }
        }
        std::sort(out->local_by_usage.rbegin(), out->local_by_usage.rend());
    }

    // Where is the requested memory actually backed? Bytes that were meant for
    // VRAM but live in system memory have been evicted (on Linux this
    // corresponds to the GTT domain).
    const RmtVirtualAllocationList& allocations = snapshot->virtual_allocation_list;
    double                          fragmentation_sum = 0.0;
    std::vector<uint64_t>           allocation_sizes;

    for (int i = 0; i < allocations.allocation_count; ++i)
    {
        const RmtVirtualAllocation* allocation                      = &allocations.allocation_details[i];
        uint64_t                    histogram[kRmtHeapTypeCount + 1] = {};
        uint64_t                    histogram_total                 = 0;
        if (RmtVirtualAllocationGetBackingStorageHistogram(snapshot, allocation, histogram, &histogram_total) == kRmtOk)
        {
            for (int k = 0; k <= kRmtHeapTypeCount; ++k)
                out->backed[k] += histogram[k];
        }
        fragmentation_sum += RmtVirtualAllocationGetFragmentationQuotient(allocation);
        // The size of every allocation is kept, not just mean and maximum:
        // the shape of the distribution is what matters when comparing
        // against a kernel-side view, which sees allocations, not the
        // resources placed inside them.
        allocation_sizes.push_back(static_cast<uint64_t>(allocation->size_in_4kb_page) * 4096ull);
    }

    out->allocation_count   = allocations.allocation_count;
    out->mean_fragmentation = allocations.allocation_count ? fragmentation_sum / allocations.allocation_count : 0.0;
    out->largest_resource   = RmtDataSnapshotGetLargestResourceSize(snapshot);

    // Allocation size distribution in power-of-two classes, the same
    // granularity a buddy allocator works in.
    if (!allocation_sizes.empty())
    {
        std::sort(allocation_sizes.begin(), allocation_sizes.end());
        for (uint64_t size : allocation_sizes)
        {
            out->size_total += size;
            int log2_size = 0;
            for (uint64_t v = 1; v < size && log2_size < 40; v <<= 1)
                ++log2_size;
            out->per_class[log2_size]++;
        }
        const size_t n   = allocation_sizes.size();
        out->size_median = allocation_sizes[n / 2];
        out->size_p90    = allocation_sizes[(n * 9) / 10];
        out->size_p99    = allocation_sizes[(n * 99) / 100];
    }

    // Most frequent individual resource sizes with their usage types. Useful
    // when a kernel-side trace shows many buffers of the same size and the
    // question is what kind of resource they are.
    const RmtResourceList& resources = snapshot->resource_list;
    for (int i = 0; i < resources.resource_count; ++i)
    {
        const RmtResource* resource = &resources.resources[i];
        out->usage_per_size[resource->size_in_bytes][RmtResourceGetUsageType(resource)]++;
    }
    for (const auto& entry : out->usage_per_size)
    {
        int count = 0;
        for (const auto& usage : entry.second)
            count += usage.second;
        out->top_sizes.emplace_back(count, entry.first);
    }
    std::sort(out->top_sizes.rbegin(), out->top_sizes.rend());
    if (out->top_sizes.size() > static_cast<size_t>(kTopSizeRows))
        out->top_sizes.resize(kTopSizeRows);

    RmtDataSnapshotDestroy(snapshot);
    return true;
}

// ------------------------------------------------------------------ output

void PrintTrace(const char* path, const RmtDataSet* data_set, int stored_snapshots)
{
    Heading("TRACE");
    printf("  File              %s\n", path);
    printf("  Target process    %" PRIu64 "\n", data_set->target_process_id);
    printf("  Streams           %d\n", data_set->stream_count);
    printf("  Active GPU        %u\n", data_set->active_gpu);
    printf("  Length            %s ticks\n", Grouped(data_set->maximum_timestamp).c_str());
    // Snapshots stored in the file itself, e.g. named in the RMV GUI.
    printf("  Stored snapshots  %d", stored_snapshots);
    for (int i = 0; i < stored_snapshots && i < 12; ++i)
        printf("%s%s", i ? ", " : "  (", data_set->snapshots[i].name);
    printf("%s\n", stored_snapshots > 12 ? ", ...)" : (stored_snapshots ? ")" : ""));

    Heading("MEMORY SEGMENTS");
    printf("  %-3s %-10s %-24s %12s\n", "#", "Heap", "", "Size");
    for (int i = 0; i < data_set->segment_info_count; ++i)
    {
        const RmtSegmentInfo& segment = data_set->segment_info[i];
        printf("  %-3d %-10s %-24s %12s\n", i, HeapName(segment.heap_type), HeapDescription(segment.heap_type),
               Size(segment.size).c_str());
    }
}

void PrintSummary(const std::vector<PointReport>& reports)
{
    Heading("TIMELINE SUMMARY");
    printf("  One row per point in time: memory requested per heap, where the process's\n");
    printf("  memory is actually backed, and the allocation count.\n\n");
    printf("  %5s | %-26s | %-26s | %-19s\n", "", "     Requested (GiB)", "      Backing (GiB)", "   Allocations");
    printf("  %5s | %8s %8s %8s | %8s %8s %8s | %7s %11s\n",
           "trace", "Local", "Invis", "System", "Local", "System", "unbacked", "count", "largest");
    printf("  ------+----------------------------+----------------------------+--------------------\n");
    for (const PointReport& r : reports)
    {
        printf("  %4d%% | %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f | %7d %11s\n",
               r.percent,
               ToGiB(r.heap_status[0].total_virtual_memory_requested),
               ToGiB(r.heap_status[1].total_virtual_memory_requested),
               ToGiB(r.heap_status[2].total_virtual_memory_requested),
               ToGiB(r.backed[kRmtHeapTypeLocal]),
               ToGiB(r.backed[kRmtHeapTypeSystem]),
               ToGiB(r.backed[kRmtHeapTypeCount]),
               r.allocation_count,
               Size(r.largest_resource).c_str());
    }
}

void PrintDetails(const PointReport& r, int index, int points)
{
    char title[128];
    snprintf(title, sizeof(title), "POINT %d OF %d  -  %d %% OF THE TRACE  (t = %s)", index, points, r.percent,
             Grouped(r.timestamp).c_str());
    Heading(title);

    // Heap status.
    printf("\n  Heaps (GiB)      requested    bound   mapped    allocs   mean alloc    max alloc  resources\n");
    for (int h = 0; h < 3; ++h)
    {
        if (!r.heap_valid[h])
            continue;
        const RmtSegmentStatus& s = r.heap_status[h];
        printf("    %-12s %11.3f %8.3f %8.3f %9" PRIu64 " %12s %12s %10" PRIu64 "\n",
               HeapName(kHeaps[h]),
               ToGiB(s.total_virtual_memory_requested),
               ToGiB(s.total_bound_virtual_memory),
               ToGiB(s.total_physical_mapped_by_process),
               s.allocation_count,
               Size(s.mean_allocation_size).c_str(),
               Size(s.max_allocation_size).c_str(),
               s.resource_count);
    }

    // Local heap by usage.
    if (!r.local_by_usage.empty())
    {
        printf("\n  Local heap by usage\n");
        for (size_t k = 0; k < r.local_by_usage.size() && k < static_cast<size_t>(kUsageRows); ++k)
        {
            const double share = Share(r.local_by_usage[k].first, r.local_usage_total);
            printf("    %-24s %10s  %5.1f %%  %s\n", UsageName(r.local_by_usage[k].second),
                   Size(r.local_by_usage[k].first).c_str(), share * 100.0, Bar(share).c_str());
        }
        if (r.local_by_usage.size() > static_cast<size_t>(kUsageRows))
            printf("    (%zu more usage types)\n", r.local_by_usage.size() - kUsageRows);
    }

    // Backing.
    const uint64_t backed_total = r.backed[kRmtHeapTypeLocal] + r.backed[kRmtHeapTypeInvisible] +
                                  r.backed[kRmtHeapTypeSystem] + r.backed[kRmtHeapTypeCount];
    printf("\n  Backing (where the process's memory actually lives)\n");
    const std::pair<const char*, uint64_t> backing[] = {
        {"Local", r.backed[kRmtHeapTypeLocal]},
        {"Invisible", r.backed[kRmtHeapTypeInvisible]},
        {"System", r.backed[kRmtHeapTypeSystem]},
        {"unbacked", r.backed[kRmtHeapTypeCount]},
    };
    for (const auto& b : backing)
    {
        const double share = Share(b.second, backed_total);
        printf("    %-24s %10s  %5.1f %%  %s\n", b.first, Size(b.second).c_str(), share * 100.0, Bar(share).c_str());
    }

    // Allocations.
    printf("\n  Allocations\n");
    printf("    count %d, mean fragmentation %.4f, largest resource %s\n", r.allocation_count, r.mean_fragmentation,
           Size(r.largest_resource).c_str());
    if (!r.per_class.empty())
    {
        printf("    size: median %s, p90 %s, p99 %s, total %s\n", Size(r.size_median).c_str(), Size(r.size_p90).c_str(),
               Size(r.size_p99).c_str(), Size(r.size_total).c_str());

        // "order" is relative to 4 KiB pages, as in the Linux kernel.
        printf("\n    order  up to        count    share\n");
        for (const auto& entry : r.per_class)
        {
            const double share = Share(static_cast<uint64_t>(entry.second), static_cast<uint64_t>(r.allocation_count));
            printf("    %5d  %-10s %6d  %5.1f %%  %s\n", entry.first - 12, Size(1ull << entry.first).c_str(), entry.second,
                   share * 100.0, Bar(share).c_str());
        }
    }

    // Most frequent resource sizes.
    if (!r.top_sizes.empty())
    {
        printf("\n  Most frequent resource sizes\n");
        printf("     count         size        total   usage\n");
        for (const auto& entry : r.top_sizes)
        {
            const uint64_t size = entry.second;

            // Usage types for this size, most frequent first.
            std::vector<std::pair<int, int>> usages;  // (count, usage)
            for (const auto& usage : r.usage_per_size.at(size))
                usages.emplace_back(usage.second, usage.first);
            std::sort(usages.rbegin(), usages.rend());

            std::string usage_text;
            for (const auto& usage : usages)
            {
                if (!usage_text.empty())
                    usage_text += ", ";
                usage_text += std::string(UsageName(usage.second)) + " " + std::to_string(usage.first);
            }
            printf("    %6d  x %10s  = %10s   %s\n", entry.first, Size(size).c_str(),
                   Size(static_cast<uint64_t>(entry.first) * size).c_str(), usage_text.c_str());
        }
    }
}

// ------------------------------------------------------- read-only loading

// RMV's backend is written for the GUI, which stores named snapshots INSIDE
// the trace file. When it loads a writable file, it works on a ".bak" copy
// next to it and writes every snapshot added with RmtDataSetAddSnapshot()
// back into the original: 35 bytes per snapshot for a RADV trace, appended at
// the end. rmv-report adds one snapshot per point in time, so every run would
// modify its input. The backend skips all of that only when the file is not
// writable (access(W_OK) fails), so the trace is loaded from a private
// read-only copy instead of the original.
class ReadOnlyCopy
{
public:
    ~ReadOnlyCopy()
    {
        if (!path_.empty())
        {
            chmod(path_.c_str(), S_IRUSR | S_IWUSR);
            unlink(path_.c_str());
        }
        if (!dir_.empty())
            rmdir(dir_.c_str());
    }

    // Returns false and prints an error if the copy cannot be made.
    bool Create(const char* source)
    {
        const char* tmp = getenv("TMPDIR");
        std::string pattern = std::string((tmp && *tmp) ? tmp : "/tmp") + "/rmv-report.XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (mkdtemp(buffer.data()) == nullptr)
        {
            fprintf(stderr, "error: cannot create a temporary directory in %s: %s\n", pattern.c_str(), strerror(errno));
            return false;
        }
        dir_ = buffer.data();

        const char* base = strrchr(source, '/');
        path_            = dir_ + "/" + (base ? base + 1 : source);

        FILE* in  = fopen(source, "rb");
        FILE* out = in ? fopen(path_.c_str(), "wb") : nullptr;
        bool  ok  = in && out;
        std::vector<char> chunk(1 << 20);
        while (ok)
        {
            const size_t n = fread(chunk.data(), 1, chunk.size(), in);
            if (n > 0 && fwrite(chunk.data(), 1, n, out) != n)
                ok = false;
            if (n < chunk.size())
            {
                ok = ok && !ferror(in);
                break;
            }
        }
        if (in)
            fclose(in);
        if (out && fclose(out) != 0)
            ok = false;
        if (!ok)
        {
            fprintf(stderr, "error: cannot copy %s to %s: %s\n", source, path_.c_str(), strerror(errno));
            return false;
        }
        // Read-only for everyone, so that access(W_OK) fails in the backend.
        chmod(path_.c_str(), S_IRUSR);
        return true;
    }

    const char* Path() const
    {
        return path_.c_str();
    }

private:
    std::string dir_;
    std::string path_;
};

// --------------------------------------------------------------- arguments

// The backend only returns numeric codes; translate the ones a user can
// actually run into when loading a file.
const char* ErrorText(RmtErrorCode code)
{
    switch (code)
    {
    case kRmtErrorInvalidPath:            return "invalid path";
    // main() has already checked that the file is readable, so at this point
    // the parser did not recognize the format.
    case kRmtErrorFileNotOpen:            return "not a recognized .rmv trace";
    case kRmtErrorFileAccessFailed:       return "file could not be accessed";
    case kRmtErrorMalformedData:          return "malformed data (truncated or not an .rmv trace?)";
    case kRmtEof:
    case kRmtEndOfFile:                   return "unexpected end of file (truncated trace?)";
    case kRmtErrorTraceFileNotSupported:  return "trace file format not supported";
    case kRmtErrorOutOfMemory:            return "out of memory";
    case kRmtErrorPageTableSizeExceeded:  return "page table size exceeded";
    default:                              return "unknown error";
    }
}

void PrintUsage(const char* program, FILE* out)
{
    fprintf(out,
            "Usage: %s [options] <trace.rmv>\n"
            "\n"
            "Summarize a Radeon Memory Visualizer trace without the GUI.\n"
            "\n"
            "Options:\n"
            "  -n, --points N   number of evenly spaced points in time (default %d, max %d)\n"
            "  -s, --summary    print only the trace info and the timeline summary\n"
            "  -h, --help       show this help\n"
            "\n"
            "For backward compatibility the number of points may also be given as a\n"
            "second positional argument: %s <trace.rmv> N\n",
            program, kDefaultPoints, kMaxPoints, program);
}

bool ParsePoints(const char* text, int* points)
{
    errno       = 0;
    char* end   = nullptr;
    long  value = strtol(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > kMaxPoints)
        return false;
    *points = static_cast<int>(value);
    return true;
}
}  // namespace

int main(int argc, char** argv)
{
    const char* trace_path   = nullptr;
    int         points       = kDefaultPoints;
    bool        points_set   = false;
    bool        summary_only = false;

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
        {
            PrintUsage(argv[0], stdout);
            return 0;
        }
        if (strcmp(arg, "-s") == 0 || strcmp(arg, "--summary") == 0)
        {
            summary_only = true;
            continue;
        }
        if (strcmp(arg, "-n") == 0 || strcmp(arg, "--points") == 0)
        {
            if (i + 1 >= argc || !ParsePoints(argv[i + 1], &points))
            {
                fprintf(stderr, "error: %s expects a number between 1 and %d\n", arg, kMaxPoints);
                return 2;
            }
            points_set = true;
            ++i;
            continue;
        }
        if (arg[0] == '-' && arg[1] != '\0')
        {
            fprintf(stderr, "error: unknown option %s\n\n", arg);
            PrintUsage(argv[0], stderr);
            return 2;
        }
        if (trace_path == nullptr)
        {
            trace_path = arg;
        }
        else if (!points_set && ParsePoints(arg, &points))
        {
            points_set = true;
        }
        else
        {
            fprintf(stderr, "error: unexpected argument %s\n\n", arg);
            PrintUsage(argv[0], stderr);
            return 2;
        }
    }

    if (trace_path == nullptr)
    {
        PrintUsage(argv[0], stderr);
        return 2;
    }

    FILE* probe = fopen(trace_path, "rb");
    if (probe == nullptr)
    {
        fprintf(stderr, "error: cannot open %s: %s\n", trace_path, strerror(errno));
        return 1;
    }
    fclose(probe);

    ReadOnlyCopy copy;
    if (!copy.Create(trace_path))
        return 1;

    const RmtErrorCode load_error = RmtTraceLoaderTraceLoad(copy.Path());
    if (load_error != kRmtOk)
    {
        fprintf(stderr, "error: loading %s failed: %s (code 0x%08x)\n", trace_path, ErrorText(load_error),
                static_cast<unsigned>(load_error));
        return 1;
    }

    RmtDataSet* data_set = RmtTraceLoaderGetDataSet();
    if (data_set == nullptr || !RmtTraceLoaderDataSetValid())
    {
        fprintf(stderr, "error: %s was loaded but contains no valid data set\n", trace_path);
        return 1;
    }
    if (!data_set->flags.read_only)
    {
        // Must never happen: the backend would write snapshots into the copy.
        fprintf(stderr, "error: the backend did not open the trace read-only\n");
        RmtTraceLoaderClearTrace();
        return 1;
    }
    // Count before this run adds its own points.
    const int stored_snapshots = data_set->snapshot_count;

    // The first point is deliberately not placed at t = 0: right after the
    // start nothing has been allocated yet, and a report of empty heaps is
    // easily misread.
    std::vector<PointReport> reports;
    std::vector<int>         indices;
    for (int p = 1; p <= points; ++p)
    {
        PointReport report;
        if (Measure(data_set, p, points, &report))
        {
            reports.push_back(std::move(report));
            indices.push_back(p);
        }
    }

    PrintTrace(trace_path, data_set, stored_snapshots);
    PrintSummary(reports);
    if (!summary_only)
    {
        for (size_t k = 0; k < reports.size(); ++k)
            PrintDetails(reports[k], indices[k], points);
    }

    RmtTraceLoaderClearTrace();
    return 0;
}
