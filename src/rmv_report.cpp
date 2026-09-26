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
// For a number of evenly spaced points in time the report shows, per heap,
// how much memory was requested, bound and mapped; where the process's memory
// is actually backed (local VRAM vs. system memory); the distribution of
// allocation sizes in power-of-two classes; and the most frequent resource
// sizes together with their usage types.

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
const int kDefaultPoints = 10;
const int kMaxPoints     = 1000;

std::string MiB(uint64_t bytes)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%9.2f", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buffer;
}

std::string GiB(uint64_t bytes)
{
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%6.3f", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buffer;
}

const char* HeapName(RmtHeapType heap)
{
    switch (heap)
    {
    case kRmtHeapTypeLocal:     return "Local (VRAM, CPU-visible)";
    case kRmtHeapTypeInvisible: return "Invisible (VRAM, not CPU-visible)";
    case kRmtHeapTypeSystem:    return "System (host memory)";
    default:                    return "?";
    }
}

const char* HeapShortName(RmtHeapType heap)
{
    switch (heap)
    {
    case kRmtHeapTypeLocal:     return "Local";
    case kRmtHeapTypeInvisible: return "Invis";
    case kRmtHeapTypeSystem:    return "System";
    default:                    return "?";
    }
}

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
    const char* trace_path = nullptr;
    int         points     = kDefaultPoints;
    bool        points_set = false;

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];
        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
        {
            PrintUsage(argv[0], stdout);
            return 0;
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

    const RmtErrorCode load_error = RmtTraceLoaderTraceLoad(trace_path);
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

    printf("================ Trace ================\n");
    printf("  File                  %s\n", trace_path);
    printf("  Target process ID     %" PRIu64 "\n", data_set->target_process_id);
    printf("  Streams               %d\n", data_set->stream_count);
    printf("  Segments              %d\n", data_set->segment_info_count);
    printf("  Active GPU            %u\n", data_set->active_gpu);
    printf("  Maximum timestamp     %" PRIu64 "\n", data_set->maximum_timestamp);

    // Hardware segment sizes: how much memory of each kind exists at all.
    for (int i = 0; i < data_set->segment_info_count; ++i)
    {
        const RmtSegmentInfo& segment = data_set->segment_info[i];
        printf("  Segment %d: %-38s %s GiB\n", i, HeapName(segment.heap_type), GiB(segment.size).c_str());
    }

    // Spread the points evenly over the trace. The first point is deliberately
    // not placed at t = 0: right after the start nothing has been allocated
    // yet, and a report of empty heaps is easily misread.
    printf("\n================ Timeline ================\n");
    printf("%-8s %-7s %10s %10s %10s %10s %9s %9s %9s\n",
           "", "Heap", "req_GiB", "bound_GiB", "mapped_GiB", "allocs", "mean_MiB", "max_MiB", "resources");

    const RmtHeapType heaps[] = {kRmtHeapTypeLocal, kRmtHeapTypeInvisible, kRmtHeapTypeSystem};

    for (int p = 1; p <= points; ++p)
    {
        const uint64_t timestamp = (data_set->maximum_timestamp * p) / points;

        char name[64];
        snprintf(name, sizeof(name), "p%02d", p);

        RmtSnapshotPoint* snapshot_point = nullptr;
        if (RmtDataSetAddSnapshot(data_set, name, timestamp, &snapshot_point) != kRmtOk || snapshot_point == nullptr)
        {
            fprintf(stderr, "  point %d: could not be created\n", p);
            continue;
        }

        // Do NOT move this to the stack. RmtDataSnapshot is several megabytes
        // large; as a local variable it overflows the default stack before the
        // first statement of main() runs.
        std::vector<uint8_t> snapshot_storage(sizeof(RmtDataSnapshot), 0);
        RmtDataSnapshot*     snapshot = reinterpret_cast<RmtDataSnapshot*>(snapshot_storage.data());
        if (RmtDataSetGenerateSnapshot(data_set, snapshot_point, snapshot) != kRmtOk)
        {
            fprintf(stderr, "  point %d: snapshot generation failed\n", p);
            continue;
        }

        printf("--- %3d %% of the trace  (t = %" PRIu64 ") ---\n", (p * 100) / points, timestamp);

        for (RmtHeapType heap : heaps)
        {
            RmtSegmentStatus status;
            memset(&status, 0, sizeof(status));
            if (RmtDataSnapshotGetSegmentStatus(snapshot, heap, &status) != kRmtOk)
                continue;

            printf("%-8s %-7s %10s %10s %10s %10" PRIu64 " %9s %9s %9" PRIu64 "\n",
                   "",
                   HeapShortName(heap),
                   GiB(status.total_virtual_memory_requested).c_str(),
                   GiB(status.total_bound_virtual_memory).c_str(),
                   GiB(status.total_physical_mapped_by_process).c_str(),
                   status.allocation_count,
                   MiB(status.mean_allocation_size).c_str(),
                   MiB(status.max_allocation_size).c_str(),
                   status.resource_count);

            // Breakdown by usage type, local heap only: shows which kind of
            // resource (textures, buffers, command buffers, ...) dominates VRAM.
            if (heap == kRmtHeapTypeLocal)
            {
                std::vector<std::pair<uint64_t, int>> by_usage;
                for (int u = 0; u < kRmtResourceUsageTypeCount; ++u)
                    if (status.physical_bytes_per_resource_usage[u] > 0)
                        by_usage.emplace_back(status.physical_bytes_per_resource_usage[u], u);
                std::sort(by_usage.rbegin(), by_usage.rend());

                printf("%-8s %-7s by usage:", "", "");
                for (size_t k = 0; k < by_usage.size() && k < 6; ++k)
                    printf("  %s %s",
                           RmtGetResourceUsageTypeNameFromResourceUsageType(static_cast<RmtResourceUsageType>(by_usage[k].second)),
                           GiB(by_usage[k].first).c_str());
                printf("\n");
            }
        }

        // Where is the requested memory actually backed? Bytes that were meant
        // for VRAM but live in system memory have been evicted (on Linux this
        // corresponds to the GTT domain).
        const RmtVirtualAllocationList& allocations = snapshot->virtual_allocation_list;
        uint64_t                        backed[kRmtHeapTypeCount + 1] = {};
        double                          fragmentation_sum             = 0.0;
        int                             fragmentation_count           = 0;
        std::vector<uint64_t>           allocation_sizes;

        for (int i = 0; i < allocations.allocation_count; ++i)
        {
            const RmtVirtualAllocation* allocation              = &allocations.allocation_details[i];
            uint64_t                    histogram[kRmtHeapTypeCount + 1] = {};
            uint64_t                    histogram_total                 = 0;
            if (RmtVirtualAllocationGetBackingStorageHistogram(snapshot, allocation, histogram, &histogram_total) == kRmtOk)
            {
                for (int k = 0; k <= kRmtHeapTypeCount; ++k)
                    backed[k] += histogram[k];
            }
            fragmentation_sum += RmtVirtualAllocationGetFragmentationQuotient(allocation);
            ++fragmentation_count;
            // The size of every allocation is kept, not just mean and maximum:
            // the shape of the distribution is what matters when comparing
            // against a kernel-side view, which sees allocations, not the
            // resources placed inside them.
            allocation_sizes.push_back(static_cast<uint64_t>(allocation->size_in_4kb_page) * 4096ull);
        }

        printf("%-8s %-7s Local %s  Invisible %s  System %s  unbacked %s GiB\n",
               "", "backing",
               GiB(backed[kRmtHeapTypeLocal]).c_str(),
               GiB(backed[kRmtHeapTypeInvisible]).c_str(),
               GiB(backed[kRmtHeapTypeSystem]).c_str(),
               GiB(backed[kRmtHeapTypeCount]).c_str());
        printf("%-8s %-7s allocations %d,  mean fragmentation %.4f,  largest resource %s MiB\n",
               "", "",
               allocations.allocation_count,
               fragmentation_count ? fragmentation_sum / fragmentation_count : 0.0,
               MiB(RmtDataSnapshotGetLargestResourceSize(snapshot)).c_str());

        // Allocation size distribution in power-of-two classes, the same
        // granularity a buddy allocator works in. "order" is relative to 4 KiB
        // pages, as in the Linux kernel.
        if (!allocation_sizes.empty())
        {
            std::sort(allocation_sizes.begin(), allocation_sizes.end());
            std::map<int, int> per_class;  // log2(size, rounded up) -> count
            uint64_t           total_bytes = 0;
            for (uint64_t size : allocation_sizes)
            {
                total_bytes += size;
                int log2_size = 0;
                for (uint64_t v = 1; v < size && log2_size < 40; v <<= 1)
                    ++log2_size;
                per_class[log2_size]++;
            }
            const size_t n = allocation_sizes.size();
            printf("%-8s %-7s allocation sizes: median %s MiB, p90 %s MiB, p99 %s MiB, total %s GiB\n",
                   "", "",
                   MiB(allocation_sizes[n / 2]).c_str(),
                   MiB(allocation_sizes[(n * 9) / 10]).c_str(),
                   MiB(allocation_sizes[(n * 99) / 100]).c_str(),
                   GiB(total_bytes).c_str());
            for (const auto& entry : per_class)
            {
                const double mib = static_cast<double>(1ull << entry.first) / 1048576.0;
                printf("%-8s %-7s   up to %9.3f MiB (order %2d): %4d allocations\n",
                       "", "", mib, entry.first - 12, entry.second);
            }
        }

        // Most frequent individual resource sizes with their usage types.
        // Useful when a kernel-side trace shows many buffers of the same size
        // and the question is what kind of resource they are.
        const RmtResourceList&                 resources = snapshot->resource_list;
        std::map<uint64_t, std::map<int, int>> by_size;  // size -> (usage type -> count)
        for (int i = 0; i < resources.resource_count; ++i)
        {
            const RmtResource* resource = &resources.resources[i];
            by_size[resource->size_in_bytes][RmtResourceGetUsageType(resource)]++;
        }

        std::vector<std::pair<int, uint64_t>> most_frequent;  // (count, size)
        for (const auto& entry : by_size)
        {
            int count = 0;
            for (const auto& usage : entry.second)
                count += usage.second;
            most_frequent.emplace_back(count, entry.first);
        }
        std::sort(most_frequent.rbegin(), most_frequent.rend());

        printf("%-8s %-7s most frequent resource sizes:\n", "", "");
        for (size_t k = 0; k < most_frequent.size() && k < 8; ++k)
        {
            const uint64_t size = most_frequent[k].second;
            printf("%-8s %-7s   %7d x %8s KiB  =%7.2f GiB  ", "", "",
                   most_frequent[k].first,
                   std::to_string(size / 1024).c_str(),
                   static_cast<double>(most_frequent[k].first) * size / (1024.0 * 1024.0 * 1024.0));
            for (const auto& usage : by_size[size])
                printf(" %s:%d",
                       RmtGetResourceUsageTypeNameFromResourceUsageType(static_cast<RmtResourceUsageType>(usage.first)),
                       usage.second);
            printf("\n");
        }

        RmtDataSnapshotDestroy(snapshot);
    }

    RmtTraceLoaderClearTrace();
    return 0;
}
