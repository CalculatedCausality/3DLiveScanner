// Private implementation, included inside namespace recon after Chunk/Allocator.
// Record identity is stable; only its separately owned payload can be paged.
struct PagingFailure {
    uint32_t reason;
    Tango3DR_Status status;
    PagingFailure(uint32_t r, Tango3DR_Status s) : reason(r), status(s) {}
};
class Pager;
struct PagedRecord {
    Pager& owner;
    Chunk* data = nullptr;
    uint32_t slot = UINT32_MAX;
    uint32_t pins = 0;
    uint64_t checksum = 0;
    bool backed = false;
    PagedRecord* previous = nullptr;
    PagedRecord* next = nullptr;
    explicit PagedRecord(Pager& p) : owner(p) {}
    ~PagedRecord();
    PagedRecord(const PagedRecord&) = delete;
    PagedRecord& operator=(const PagedRecord&) = delete;
};
class Pager {
    int fd_ = -1;
    uint64_t capacity_, slot_limit_, resident_ = 0, peak_ = 0;
    uint64_t file_bytes_ = 0, reads_ = 0, writes_ = 0, evictions_ = 0;
    uint32_t slots_ = 0;
    Vector<uint32_t> free_;
    PagedRecord* newest_ = nullptr;
    PagedRecord* oldest_ = nullptr;
    bool broken_ = false;
#ifdef RECONSTRUCTION_CORE_TESTING
    // One-shot faults, optionally after a real partial I/O. No production API.
    int fault_ = 0;
    int64_t fault_after_ = 0;
    bool fault(int kind) {
        if (fault_ != kind) return false;
        if (fault_after_-- > 0) return false;
        fault_ = 0; return true;
    }
#endif
    void detach(PagedRecord& r) noexcept {
        if (r.previous) r.previous->next = r.next; else newest_ = r.next;
        if (r.next) r.next->previous = r.previous; else oldest_ = r.previous;
        r.previous = r.next = nullptr;
    }
    void front(PagedRecord& r) noexcept {
        r.previous = nullptr; r.next = newest_;
        if (newest_) newest_->previous = &r; else oldest_ = &r;
        newest_ = &r;
    }
    void touch(PagedRecord& r) noexcept {
        if (newest_ != &r) { detach(r); front(r); }
    }
    static uint64_t checksum(const Chunk& c) noexcept {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&c);
        uint64_t hash = UINT64_C(14695981039346656037);
        for (size_t i = 0; i < sizeof(Chunk); ++i) { hash ^= bytes[i]; hash *= UINT64_C(1099511628211); }
        return hash;
    }
    void io(bool write, void* data, off_t offset) {
        size_t done = 0;
        while (done < sizeof(Chunk)) {
            ssize_t n = write ? ::pwrite(fd_,static_cast<uint8_t*>(data)+done,sizeof(Chunk)-done,offset+off_t(done)) :
                                ::pread(fd_,static_cast<uint8_t*>(data)+done,sizeof(Chunk)-done,offset+off_t(done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                if (!write) broken_ = true;
                throw PagingFailure(write ? SCANNER_RECONSTRUCTION_FAILURE_CACHE_WRITE : SCANNER_RECONSTRUCTION_FAILURE_CACHE_READ,TANGO_3DR_ERROR);
            }
            done += size_t(n);
            if (write) file_bytes_ = std::max(file_bytes_,uint64_t(offset)+done);
        }
    }
    void save(PagedRecord& r) {
        if (r.backed) return;
        if (r.slot == UINT32_MAX) {
            if (!free_.empty()) { r.slot = free_.back(); free_.pop_back(); }
            else {
                if (slots_ >= slot_limit_)
                    throw PagingFailure(SCANNER_RECONSTRUCTION_FAILURE_BACKING_LIMIT,TANGO_3DR_INSUFFICIENT_SPACE);
                r.slot = slots_++;
            }
        }
        off_t offset = off_t(uint64_t(r.slot)*sizeof(Chunk));
#ifdef RECONSTRUCTION_CORE_TESTING
        if (fault(1)) { // simulate ENOSPC/EIO after a partial successful write
            ssize_t n = ::pwrite(fd_,r.data,37,offset);
            if (n > 0) file_bytes_ = std::max(file_bytes_,uint64_t(offset)+uint64_t(n));
            throw PagingFailure(SCANNER_RECONSTRUCTION_FAILURE_CACHE_WRITE,TANGO_3DR_ERROR);
        }
#endif
        io(true,r.data,offset);
        r.checksum = checksum(*r.data);
        r.backed = true; ++writes_;
    }
    void removePayload(PagedRecord& r) noexcept {
        detach(r); delete r.data; r.data = nullptr; --resident_;
    }
    void room() {
        while (resident_ >= capacity_) {
            PagedRecord* victim = oldest_;
            while (victim && victim->pins) victim = victim->previous;
            if (!victim) throw PagingFailure(SCANNER_RECONSTRUCTION_FAILURE_MEMORY_LIMIT,TANGO_3DR_INSUFFICIENT_SPACE);
            // Failure retains the only valid resident payload. Never free it
            // before checked write completion and checksum publication.
            save(*victim); removePayload(*victim); ++evictions_;
        }
    }
    void allocate(PagedRecord& r, const Chunk* source) {
        room();
        allocationPoint();
        r.data = source ? new Chunk(*source) : new Chunk();
        ++resident_; peak_ = std::max(peak_,resident_); front(r);
    }
public:
    const uint64_t resident_budget, backing_budget;
    const uint32_t logical_limit;
    Pager(uint64_t resident, uint64_t backing, uint32_t logical, uint32_t frame_limit)
        : capacity_(resident/sizeof(Chunk)),
          slot_limit_(std::min(backing/sizeof(Chunk),uint64_t(logical)+frame_limit)),
          resident_budget(resident), backing_budget(backing), logical_limit(logical) {
        free_.reserve(size_t(slot_limit_)); // bounded; release/rollback never allocates
    }
    ~Pager() { assert(!resident_ && !newest_ && !oldest_); if (fd_ >= 0) ::close(fd_); }
    void open(const char* directory) {
        std::string name(directory);
        name += "/scanner-tsdf-XXXXXX";
        Vector<char> path(name.begin(),name.end()); path.push_back('\0');
        int fd = ::mkstemp(path.data());
        if (fd < 0) throw PagingFailure(SCANNER_RECONSTRUCTION_FAILURE_CACHE_OPEN,TANGO_3DR_ERROR);
        int flags = ::fcntl(fd,F_GETFD);
        bool flags_ok = flags >= 0 && ::fcntl(fd,F_SETFD,flags|FD_CLOEXEC) == 0;
        int unlinked = ::unlink(path.data());
        if (!flags_ok || unlinked != 0) {
            ::close(fd);
            // Only the unique file we just created, never caller-owned files.
            if (unlinked != 0) ::unlink(path.data());
            throw PagingFailure(SCANNER_RECONSTRUCTION_FAILURE_CACHE_OPEN,TANGO_3DR_ERROR);
        }
        fd_ = fd;
    }
    bool broken() const noexcept { return broken_; }
    void cleared() noexcept { broken_ = false; } // free slots/extent retained for reuse
    void release(PagedRecord& r) noexcept {
        assert(r.pins == 0);
        if (r.data) removePayload(r);
        if (r.slot != UINT32_MAX) { free_.push_back(r.slot); r.slot = UINT32_MAX; }
    }
    void pin(PagedRecord& r) {
        if (broken_) throw PagingFailure(SCANNER_RECONSTRUCTION_FAILURE_CACHE_READ,TANGO_3DR_ERROR);
        if (!r.data) {
            if (!r.backed) { broken_ = true; throw PagingFailure(SCANNER_RECONSTRUCTION_FAILURE_CACHE_READ,TANGO_3DR_ERROR); }
            allocate(r,nullptr); // loading scratch is charged to resident budget
            try {
#ifdef RECONSTRUCTION_CORE_TESTING
                if (fault(2)) { // partial read failure
                    ssize_t partial = ::pread(fd_,r.data,37,off_t(uint64_t(r.slot)*sizeof(Chunk)));
                    (void)partial;
                    broken_ = true;
                    throw PagingFailure(SCANNER_RECONSTRUCTION_FAILURE_CACHE_READ,TANGO_3DR_ERROR);
                }
#endif
                io(false,r.data,off_t(uint64_t(r.slot)*sizeof(Chunk)));
#ifdef RECONSTRUCTION_CORE_TESTING
                if (fault(3)) reinterpret_cast<uint8_t*>(r.data)[19] ^= 1;
#endif
                if (checksum(*r.data) != r.checksum) {
                    broken_ = true;
                    throw PagingFailure(SCANNER_RECONSTRUCTION_FAILURE_CACHE_READ,TANGO_3DR_ERROR);
                }
                ++reads_;
            } catch (...) { removePayload(r); throw; }
        }
        touch(r); ++r.pins;
    }
    void unpin(PagedRecord& r) noexcept { assert(r.pins); --r.pins; }
    std::shared_ptr<PagedRecord> create(const Chunk* source) {
        auto record = std::allocate_shared<PagedRecord>(Allocator<PagedRecord>(),*this);
        allocate(*record,source);
        return record;
    }
    void modified(PagedRecord& r) noexcept { assert(r.data && r.pins); r.backed = false; }
    void stats(ScannerReconstruction_PagingStats& out) const noexcept {
        out.resident_chunks = resident_; out.peak_resident_chunks = peak_;
        out.resident_bytes = resident_*sizeof(Chunk); out.peak_resident_bytes = peak_*sizeof(Chunk);
        out.backing_bytes = file_bytes_; out.backing_live_bytes = (slots_-free_.size())*sizeof(Chunk);
        out.reads = reads_; out.writes = writes_; out.evictions = evictions_;
        out.resident_budget_bytes = resident_budget; out.backing_budget_bytes = backing_budget;
        out.requires_replay = broken_ ? 1 : 0;
    }
#ifdef RECONSTRUCTION_CORE_TESTING
    void setFault(int kind, int64_t after) { fault_ = kind; fault_after_ = after; }
    int descriptor() const { return fd_; }
#endif
};
inline PagedRecord::~PagedRecord() { owner.release(*this); }
struct PagePin {
    PagedRecord* record = nullptr;
    PagePin() = default;
    PagePin(const PagePin&) = delete;
    PagePin& operator=(const PagePin&) = delete;
    ~PagePin() { reset(); }
    void reset() noexcept {
        if (record) { record->owner.unpin(*record); record = nullptr; }
    }
    void set(PagedRecord& next) {
        reset(); next.owner.pin(next); record = &next;
    }
};
typedef Map<Index,std::shared_ptr<PagedRecord> > PagedVolume;
