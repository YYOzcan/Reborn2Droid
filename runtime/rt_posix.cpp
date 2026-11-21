// POSIX layer: files, directories, descriptors, sockets, memory mapping, dynamic loading, process info.
// Hosts are Linux / Android, so guest file descriptors are real host descriptors; only structure
// layouts (32-bit bionic) and a few flag values are translated.
#include "rt.h"
#include "host.h"
#include "thread.h"
#include <cerrno>
#include <cstring>
#include <mutex>
#include <map>
#include <unordered_map>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/utsname.h>
#include <sys/resource.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <utime.h>
#include <zlib.h>
#include <sys/syscall.h>

using namespace rt;
#define A(i) arg(c, i)
#define RET(v) (c->r[0] = (uint32_t)(v))
static void herr() { set_errno_from_host(); }
#define SYSRET(expr) do { long _r = (long)(expr); if (_r < 0) { herr(); RET(-1); } else RET(_r); } while (0)

// ======================================================================= paths and virtual files
namespace rt {
std::string host_path(const char *g) { return g ? std::string(g) : std::string(); }
}
std::string proc_maps_text();
static std::string cpuinfo_text() {
    return "Processor\t: ARMv7 Processor rev 0 (v7l)\nprocessor\t: 0\nBogoMIPS\t: 1000.0\n"
           "Features\t: swp half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt \n"
           "CPU implementer\t: 0x41\nCPU architecture: 7\nCPU variant\t: 0x0\nCPU part\t: 0xc0f\nCPU revision\t: 0\n\n"
           "Hardware\t: Reborn2Droid\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n";
}
static int virtual_file(const std::string &p) {
    std::string text;
    if (p == "/proc/cpuinfo") text = cpuinfo_text();
    else if (p == "/proc/self/maps" || (p.rfind("/proc/", 0) == 0 && p.find("/maps") != std::string::npos)) text = proc_maps_text();
    else if (p == "/proc/self/stat" || p == "/proc/1000/stat") {
        extern uint32_t main_stack_top();
        char buf[512];
        snprintf(buf, sizeof buf, "1000 (app_process) R 1 1000 0 0 -1 0 0 0 0 0 0 0 0 0 20 0 1 0 0 0 0 0 0 0 0 0 %u 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                 main_stack_top());
        text = buf;
    } else if (p == "/proc/meminfo") text = "MemTotal:        2097152 kB\nMemFree:         1048576 kB\nCached:           262144 kB\n";
    else if (p == "/sys/devices/system/cpu/present" || p == "/sys/devices/system/cpu/possible") text = "0-3\n";
    else return -2;
    int fd = (int)syscall(SYS_memfd_create, "vfile", 0u);
    if (fd < 0) {                                  // old kernels: an unlinked file in the cache dir
        std::string tmp = rt::cache_dir + "/.vfile" + std::to_string(rand());
        fd = ::open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) return -1;
        ::unlink(tmp.c_str());
    }
    if (::write(fd, text.data(), text.size()) < 0) { ::close(fd); return -1; }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

// ARM and x86_64 disagree on four open() flags
static int open_flags(uint32_t g) {
    int f = (int)(g & ~(040000u | 0100000u | 0200000u | 0400000u));
    if (g & 040000u) f |= O_DIRECTORY;
    if (g & 0100000u) f |= O_NOFOLLOW;
    if (g & 0400000u) f |= 0;   // O_LARGEFILE: implicit on 64-bit
    return f;
}
static uint32_t guest_flags(int f) {
    uint32_t g = (uint32_t)f & ~(uint32_t)(O_DIRECTORY | O_NOFOLLOW | O_DIRECT);
#ifdef O_LARGEFILE
    g &= ~(uint32_t)O_LARGEFILE;
#endif
    if (f & O_DIRECTORY) g |= 040000u;
    if (f & O_NOFOLLOW) g |= 0100000u;
    return g;
}

IMP(open) {
    std::string p = host_path(GSTR(A(0)));
    int vf = virtual_file(p);
    if (vf != -2) { if (vf < 0) herr(); RET(vf); return; }
    SYSRET(::open(p.c_str(), open_flags(A(1)) | O_CLOEXEC, (mode_t)A(2)));
}
IMP(close) { SYSRET(::close((int)A(0))); }
IMP(read) { SYSRET(::read((int)A(0), GPTR(A(1)), A(2))); }
IMP(write) {
    int fd = (int)A(0);
    if (fd == 1 || fd == 2) { std::string s((const char *)GPTR(A(1)), A(2)); while (!s.empty() && s.back() == '\n') s.pop_back(); logf("[stdout] %s", s.c_str()); RET(A(2)); return; }
    SYSRET(::write(fd, GPTR(A(1)), A(2)));
}
IMP(lseek) { off_t r = ::lseek((int)A(0), (off_t)(int32_t)A(1), (int)A(2)); if (r < 0) { herr(); RET(-1); } else RET((uint32_t)r); }
IMP(lseek64) { int i = 2; int64_t off = (int64_t)arg64(c, i); int wh = (int)arg(c, i); off_t r = ::lseek((int)A(0), off, wh); if (r < 0) { herr(); ret64(c, (uint64_t)-1); } else ret64(c, (uint64_t)r); }
IMP(dup) { SYSRET(::fcntl((int)A(0), F_DUPFD_CLOEXEC, 3)); }
IMP(dup2) { SYSRET(::dup2((int)A(0), (int)A(1))); }
IMP(pipe) { int p[2]; if (::pipe2(p, O_CLOEXEC)) { herr(); RET(-1); return; } W32(A(0), p[0]); W32(A(0) + 4, p[1]); RET(0); }
IMP(fsync) { SYSRET(::fsync((int)A(0))); }
IMP(ftruncate) { SYSRET(::ftruncate((int)A(0), (off_t)(int32_t)A(1))); }
IMP(truncate) { SYSRET(::truncate(GSTR(A(0)), (off_t)(int32_t)A(1))); }
IMP(flock) { SYSRET(::flock((int)A(0), (int)A(1))); }
IMP(isatty) { RET(0); }
IMP(getdtablesize) { RET(1024); }
IMP(fcntl) {
    int fd = (int)A(0), cmd = (int)A(1); uint32_t a = A(2);
    switch (cmd) {
    case 3: { int r = ::fcntl(fd, F_GETFL); if (r < 0) { herr(); RET(-1); } else RET(guest_flags(r)); return; }
    case 4: SYSRET(::fcntl(fd, F_SETFL, open_flags(a))); return;
    case 1: case 2: case 0: case 1030: SYSRET(::fcntl(fd, cmd, (long)a)); return;   // F_GETFD F_SETFD F_DUPFD F_DUPFD_CLOEXEC
    case 5: case 6: case 7: {   // F_GETLK F_SETLK F_SETLKW (32-bit struct flock): type, whence, start, len, pid
        struct flock fl{}; fl.l_type = (short)R16(a); fl.l_whence = (short)R16(a + 2); fl.l_start = (int32_t)R32(a + 4); fl.l_len = (int32_t)R32(a + 8);
        int r = ::fcntl(fd, cmd, &fl);
        if (r < 0) { herr(); RET(-1); return; }
        if (cmd == 5) { W16(a, fl.l_type); W16(a + 2, fl.l_whence); W32(a + 4, (uint32_t)fl.l_start); W32(a + 8, (uint32_t)fl.l_len); W32(a + 12, fl.l_pid); }
        RET(r); return;
    }
    }
    logf("fcntl(%d, %d) unsupported", fd, cmd); RET(0);
}
IMP(ioctl) {
    int fd = (int)A(0); uint32_t req = A(1);
    if (req == 0x541B || req == 0x5421) { int v = req == 0x5421 ? (int)R32(A(2)) : 0; int r = ::ioctl(fd, req, &v); if (r < 0) { herr(); RET(-1); return; } if (req == 0x541B) W32(A(2), v); RET(r); return; }
    gerr(25); RET(-1);   // ENOTTY
}

// ---- stat (bionic arm struct stat, 104 bytes)
static void stat_to_guest(const struct stat &st, uint32_t g) {
    memset(GPTR(g), 0, 104);
    W32(g, (uint32_t)st.st_dev); W32(g + 12, (uint32_t)st.st_ino);
    W32(g + 16, st.st_mode); W32(g + 20, (uint32_t)st.st_nlink); W32(g + 24, st.st_uid); W32(g + 28, st.st_gid);
    W32(g + 32, (uint32_t)st.st_rdev);
    W32(g + 48, (uint32_t)st.st_size); W32(g + 52, (uint32_t)((uint64_t)st.st_size >> 32)); W32(g + 56, (uint32_t)st.st_blksize);
    W32(g + 64, (uint32_t)st.st_blocks);
    W32(g + 72, (uint32_t)st.st_atim.tv_sec); W32(g + 76, (uint32_t)st.st_atim.tv_nsec);
    W32(g + 80, (uint32_t)st.st_mtim.tv_sec); W32(g + 84, (uint32_t)st.st_mtim.tv_nsec);
    W32(g + 88, (uint32_t)st.st_ctim.tv_sec); W32(g + 92, (uint32_t)st.st_ctim.tv_nsec);
    W32(g + 96, (uint32_t)st.st_ino); W32(g + 100, (uint32_t)((uint64_t)st.st_ino >> 32));
}
IMP(stat) { struct stat st; std::string p = host_path(GSTR(A(0))); if (::stat(p.c_str(), &st)) { herr(); RET(-1); return; } stat_to_guest(st, A(1)); RET(0); }
IMP(lstat) { struct stat st; std::string p = host_path(GSTR(A(0))); if (::lstat(p.c_str(), &st)) { herr(); RET(-1); return; } stat_to_guest(st, A(1)); RET(0); }
IMP(fstat) { struct stat st; if (::fstat((int)A(0), &st)) { herr(); RET(-1); return; } stat_to_guest(st, A(1)); RET(0); }
IMP(access) { SYSRET(::access(host_path(GSTR(A(0))).c_str(), (int)A(1))); }
IMP(unlink) { SYSRET(::unlink(host_path(GSTR(A(0))).c_str())); }
IMP(remove) { SYSRET(::remove(host_path(GSTR(A(0))).c_str())); }
IMP(rename) { SYSRET(::rename(host_path(GSTR(A(0))).c_str(), host_path(GSTR(A(1))).c_str())); }
IMP(mkdir) { SYSRET(::mkdir(host_path(GSTR(A(0))).c_str(), (mode_t)A(1))); }
IMP(rmdir) { SYSRET(::rmdir(host_path(GSTR(A(0))).c_str())); }
IMP(chmod) { SYSRET(::chmod(host_path(GSTR(A(0))).c_str(), (mode_t)A(1))); }
IMP(chdir) { SYSRET(::chdir(host_path(GSTR(A(0))).c_str())); }
IMP(getcwd) {
    char buf[4096]; if (!::getcwd(buf, sizeof buf)) { herr(); RET(0); return; }
    if (!A(0)) { RET(gstrdup(buf)); return; }
    if (strlen(buf) + 1 > A(1)) { gerr(34); RET(0); return; }
    strcpy((char *)GPTR(A(0)), buf); RET(A(0));
}
IMP(readlink) {
    std::string p = host_path(GSTR(A(0)));
    if (p == "/proc/self/exe") { const char *e = "/system/bin/app_process"; size_t n = std::min<size_t>(strlen(e), A(2)); memcpy(GPTR(A(1)), e, n); RET(n); return; }
    SYSRET(::readlink(p.c_str(), (char *)GPTR(A(1)), A(2)));
}
IMP(utime) {
    if (!A(1)) { SYSRET(::utime(GSTR(A(0)), nullptr)); return; }
    struct utimbuf u{(time_t)(int32_t)R32(A(1)), (time_t)(int32_t)R32(A(1) + 4)}; SYSRET(::utime(GSTR(A(0)), &u));
}
IMP(mkstemp) { SYSRET(::mkstemp((char *)GPTR(A(0)))); }
IMP(sendfile) {
    off_t off = A(2) ? (off_t)(int32_t)R32(A(2)) : 0;
    ssize_t r = ::sendfile((int)A(0), (int)A(1), A(2) ? &off : nullptr, A(3));
    if (A(2)) W32(A(2), (uint32_t)off);
    if (r < 0) { herr(); RET(-1); } else RET(r);
}

// ---- directories: DIR* = guest handle; struct dirent (bionic): u64 ino, s64 off, u16 reclen, u8 type, char name[256]
namespace { struct GDir { DIR *d; uint32_t ent; }; std::mutex dir_mu; std::unordered_map<uint32_t, GDir> dirs; }
IMP(opendir) {
    DIR *d = ::opendir(host_path(GSTR(A(0))).c_str());
    if (!d) { herr(); RET(0); return; }
    uint32_t h = gstatic(16), ent = gstatic(280);
    std::lock_guard<std::mutex> g(dir_mu); dirs[h] = GDir{d, ent}; RET(h);
}
IMP(readdir) {
    GDir gd; { std::lock_guard<std::mutex> g(dir_mu); auto it = dirs.find(A(0)); if (it == dirs.end()) { gerr(9); RET(0); return; } gd = it->second; }
    struct dirent *e = ::readdir(gd.d);
    if (!e) { RET(0); return; }
    uint32_t g = gd.ent;
    W32(g, (uint32_t)e->d_ino); W32(g + 4, 0); W32(g + 8, 0); W32(g + 12, 0);
    W16(g + 16, 280); W8(g + 18, e->d_type);
    strncpy((char *)GPTR(g + 19), e->d_name, 255); W8(g + 19 + 255, 0);
    RET(g);
}
IMP(closedir) {
    std::lock_guard<std::mutex> g(dir_mu); auto it = dirs.find(A(0));
    if (it == dirs.end()) { RET(-1); return; }
    ::closedir(it->second.d); gfree(it->second.ent); dirs.erase(it); gfree(A(0)); RET(0);
}

// ======================================================================= stdio (guest FILE* -> host FILE*)
static std::mutex file_mu;
static std::unordered_map<uint32_t, FILE *> files;
static uint32_t sF;                                   // bionic __sF[3], 84 bytes each
extern "C" uint32_t rt_sF() { if (!sF) { sF = gstatic(84 * 3); } return sF; }
static FILE *hostf(uint32_t g) {
    if (g == rt_sF()) return stdin;
    if (g == rt_sF() + 84) return stdout;
    if (g == rt_sF() + 168) return stderr;
    std::lock_guard<std::mutex> l(file_mu); auto it = files.find(g); return it == files.end() ? nullptr : it->second;
}
static uint32_t wrap_file(FILE *f) {
    uint32_t g = gstatic(84);
    W32(g + 12, (uint32_t)fileno(f) << 16);   // bionic: short _file at offset 14
    std::lock_guard<std::mutex> l(file_mu); files[g] = f; return g;
}
IMP(fopen) {
    std::string p = host_path(GSTR(A(0)));
    int vf = virtual_file(p);
    FILE *f = vf >= 0 ? fdopen(vf, "rb") : vf == -2 ? ::fopen(p.c_str(), GSTR(A(1))) : nullptr;
    if (!f) { herr(); RET(0); return; }
    RET(wrap_file(f));
}
IMP(fdopen) { FILE *f = ::fdopen((int)A(0), GSTR(A(1))); if (!f) { herr(); RET(0); return; } RET(wrap_file(f)); }
IMP(fclose) {
    FILE *f = hostf(A(0));
    { std::lock_guard<std::mutex> l(file_mu); files.erase(A(0)); }
    int r = f ? ::fclose(f) : -1;
    RET(r);
}
IMP(fread) { FILE *f = hostf(A(3)); RET(f ? ::fread(GPTR(A(0)), A(1), A(2), f) : 0); }
IMP(fwrite) {
    FILE *f = hostf(A(3));
    if (f == stdout || f == stderr) { std::string s((const char *)GPTR(A(0)), (size_t)A(1) * A(2)); while (!s.empty() && s.back() == '\n') s.pop_back(); if (!s.empty()) logf("[stdio] %s", s.c_str()); RET(A(2)); return; }
    RET(f ? ::fwrite(GPTR(A(0)), A(1), A(2), f) : 0);
}
IMP(fseek) { FILE *f = hostf(A(0)); RET(f ? ::fseeko(f, (off_t)(int32_t)A(1), (int)A(2)) : -1); }
IMP(ftell) { FILE *f = hostf(A(0)); RET(f ? (uint32_t)::ftello(f) : (uint32_t)-1); }
IMP(fgetpos) { FILE *f = hostf(A(0)); if (!f) { RET(-1); return; } W32(A(1), (uint32_t)::ftello(f)); RET(0); }
IMP(fsetpos) { FILE *f = hostf(A(0)); RET(f ? ::fseeko(f, R32(A(1)), SEEK_SET) : -1); }
IMP(fflush) { FILE *f = A(0) ? hostf(A(0)) : nullptr; RET(::fflush(f)); }
IMP(fgets) { FILE *f = hostf(A(2)); char *r = f ? ::fgets((char *)GPTR(A(0)), (int)A(1), f) : nullptr; RET(r ? A(0) : 0); }
IMP(fputc) { FILE *f = hostf(A(1)); RET(f ? ::fputc((int)A(0), f) : -1); }
IMP(fputs) { FILE *f = hostf(A(1)); if (f == stdout || f == stderr) { logf("[stdio] %s", GSTR(A(0))); RET(1); return; } RET(f ? ::fputs(GSTR(A(0)), f) : -1); }
IMP(putc) { FILE *f = hostf(A(1)); RET(f ? ::fputc((int)A(0), f) : -1); }
IMP(getc) { FILE *f = hostf(A(0)); RET(f ? ::fgetc(f) : -1); }
IMP(fgetc) { FILE *f = hostf(A(0)); RET(f ? ::fgetc(f) : -1); }
IMP(ungetc) { FILE *f = hostf(A(1)); RET(f ? ::ungetc((int)A(0), f) : -1); }
IMP(ferror) { FILE *f = hostf(A(0)); RET(f ? ::ferror(f) : 1); }
IMP(feof) { FILE *f = hostf(A(0)); RET(f ? ::feof(f) : 1); }
IMP(fileno) { FILE *f = hostf(A(0)); RET(f ? ::fileno(f) : -1); }
IMP(setvbuf) { RET(0); }
IMP(clearerr) { FILE *f = hostf(A(0)); if (f) ::clearerr(f); }
IMP(fprintf) {
    VA va{c, 2, 0, false}; std::string s = gformat(GSTR(A(1)), va); FILE *f = hostf(A(0));
    if (f == stdout || f == stderr) { std::string t = s; while (!t.empty() && t.back() == '\n') t.pop_back(); logf("[stdio] %s", t.c_str()); }
    else if (f) ::fputs(s.c_str(), f);
    RET(s.size());
}
IMP(vfprintf) {
    VA va{c, 0, A(2), true}; std::string s = gformat(GSTR(A(1)), va); FILE *f = hostf(A(0));
    if (f == stdout || f == stderr) { std::string t = s; while (!t.empty() && t.back() == '\n') t.pop_back(); logf("[stdio] %s", t.c_str()); }
    else if (f) ::fputs(s.c_str(), f);
    RET(s.size());
}
IMP(fscanf) {
    FILE *f = hostf(A(0)); if (!f) { RET(-1); return; }
    // read one line and scan it (good enough for /proc parsing)
    char line[1024]; if (!::fgets(line, sizeof line, f)) { RET(-1); return; }
    uint32_t tmp = gstrdup(line);
    CPU c2 = *c; c2.r[0] = tmp; c2.r[1] = A(1); c2.r[2] = A(2); c2.r[3] = A(3);
    extern void imp_sscanf(CPU *); imp_sscanf(&c2);
    gfree(tmp); RET(c2.r[0]);
}
IMP(popen) { gerr(38); RET(0); }
IMP(pclose) { RET(-1); }

// ======================================================================= sockets (real host sockets)
IMP(socket) { SYSRET(::socket((int)A(0), (int)A(1) | SOCK_CLOEXEC, (int)A(2))); }
IMP(connect) { SYSRET(::connect((int)A(0), (const sockaddr *)GPTR(A(1)), A(2))); }
IMP(bind) { SYSRET(::bind((int)A(0), (const sockaddr *)GPTR(A(1)), A(2))); }
IMP(listen) { SYSRET(::listen((int)A(0), (int)A(1))); }
IMP(accept) { socklen_t l = A(2) ? R32(A(2)) : 0; int r = ::accept4((int)A(0), A(1) ? (sockaddr *)GPTR(A(1)) : nullptr, A(2) ? &l : nullptr, SOCK_CLOEXEC); if (A(2)) W32(A(2), l); if (r < 0) { herr(); RET(-1); } else RET(r); }
IMP(send) { SYSRET(::send((int)A(0), GPTR(A(1)), A(2), (int)A(3) | MSG_NOSIGNAL)); }
IMP(recv) { SYSRET(::recv((int)A(0), GPTR(A(1)), A(2), (int)A(3))); }
IMP(sendto) { SYSRET(::sendto((int)A(0), GPTR(A(1)), A(2), (int)A(3) | MSG_NOSIGNAL, A(4) ? (const sockaddr *)GPTR(A(4)) : nullptr, A(5))); }
IMP(recvfrom) { socklen_t l = A(5) ? R32(A(5)) : 0; ssize_t r = ::recvfrom((int)A(0), GPTR(A(1)), A(2), (int)A(3), A(4) ? (sockaddr *)GPTR(A(4)) : nullptr, A(5) ? &l : nullptr); if (A(5)) W32(A(5), l); if (r < 0) { herr(); RET(-1); } else RET(r); }
static void msg_from_guest(uint32_t g, msghdr &m, std::vector<iovec> &iov) {
    m.msg_name = R32(g) ? GPTR(R32(g)) : nullptr; m.msg_namelen = R32(g + 4);
    uint32_t gi = R32(g + 8), n = R32(g + 12); iov.resize(n);
    for (uint32_t k = 0; k < n; k++) { iov[k].iov_base = GPTR(R32(gi + 8 * k)); iov[k].iov_len = R32(gi + 8 * k + 4); }
    m.msg_iov = iov.data(); m.msg_iovlen = n; m.msg_control = nullptr; m.msg_controllen = 0; m.msg_flags = (int)R32(g + 24);
}
IMP(sendmsg) { msghdr m{}; std::vector<iovec> v; msg_from_guest(A(1), m, v); SYSRET(::sendmsg((int)A(0), &m, (int)A(2) | MSG_NOSIGNAL)); }
IMP(recvmsg) { msghdr m{}; std::vector<iovec> v; msg_from_guest(A(1), m, v); ssize_t r = ::recvmsg((int)A(0), &m, (int)A(2)); W32(A(1) + 4, m.msg_namelen); W32(A(1) + 20, 0); W32(A(1) + 24, m.msg_flags); if (r < 0) { herr(); RET(-1); } else RET(r); }
IMP(shutdown) { SYSRET(::shutdown((int)A(0), (int)A(1))); }
IMP(setsockopt) {
    int lvl = (int)A(1), opt = (int)A(2);
    if (lvl == SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO) && A(4) >= 8) {
        timeval tv{(time_t)(int32_t)R32(A(3)), (suseconds_t)(int32_t)R32(A(3) + 4)}; SYSRET(::setsockopt((int)A(0), lvl, opt, &tv, sizeof tv)); return;
    }
    SYSRET(::setsockopt((int)A(0), lvl, opt, GPTR(A(3)), A(4)));
}
IMP(getsockopt) {
    socklen_t l = R32(A(4));
    int lvl = (int)A(1), opt = (int)A(2);
    if (lvl == SOL_SOCKET && (opt == SO_RCVTIMEO || opt == SO_SNDTIMEO)) {
        timeval tv{}; socklen_t tl = sizeof tv; int r = ::getsockopt((int)A(0), lvl, opt, &tv, &tl);
        W32(A(3), (uint32_t)tv.tv_sec); W32(A(3) + 4, (uint32_t)tv.tv_usec); W32(A(4), 8); if (r < 0) { herr(); RET(-1); } else RET(0); return;
    }
    int r = ::getsockopt((int)A(0), lvl, opt, GPTR(A(3)), &l); W32(A(4), l); if (r < 0) { herr(); RET(-1); } else RET(r);
}
IMP(getsockname) { socklen_t l = R32(A(2)); int r = ::getsockname((int)A(0), (sockaddr *)GPTR(A(1)), &l); W32(A(2), l); if (r < 0) { herr(); RET(-1); } else RET(0); }
IMP(getpeername) { socklen_t l = R32(A(2)); int r = ::getpeername((int)A(0), (sockaddr *)GPTR(A(1)), &l); W32(A(2), l); if (r < 0) { herr(); RET(-1); } else RET(0); }
IMP(select) {
    int n = (int)A(0); uint32_t tvp = A(4);
    timeval tv{}; if (tvp) { tv.tv_sec = (int32_t)R32(tvp); tv.tv_usec = (int32_t)R32(tvp + 4); }
    int r = ::select(n, A(1) ? (fd_set *)GPTR(A(1)) : nullptr, A(2) ? (fd_set *)GPTR(A(2)) : nullptr,
                     A(3) ? (fd_set *)GPTR(A(3)) : nullptr, tvp ? &tv : nullptr);
    if (tvp) { W32(tvp, (uint32_t)tv.tv_sec); W32(tvp + 4, (uint32_t)tv.tv_usec); }
    if (r < 0) { herr(); RET(-1); } else RET(r);
}
IMP(poll) {
    // struct pollfd is identical on both sides; long waits are sliced so guest signals get delivered
    int timeout = (int)A(2);
    int64_t end = timeout > 0 ? host::mono_ns() + (int64_t)timeout * 1000000 : 0;
    for (;;) {
        int slice = timeout < 0 ? 100 : timeout == 0 ? 0 : (int)std::min<int64_t>(100, std::max<int64_t>(0, (end - host::mono_ns()) / 1000000));
        int r = ::poll((pollfd *)GPTR(A(0)), A(1), slice);
        if (r != 0 || timeout == 0 || (timeout > 0 && host::mono_ns() >= end)) { if (r < 0) { herr(); RET(-1); } else RET(r); return; }
        if (c->sigpending) { guest_poll(c); gerr(EINTR); RET(-1); return; }
    }
}
// epoll_event: ARM EABI {u32 events; u32 pad; u64 data} (16 bytes); x86_64 packs it to 12
IMP(epoll_create) { SYSRET(::epoll_create1(EPOLL_CLOEXEC)); }
IMP(epoll_ctl) {
    epoll_event e{}; if (A(3)) { e.events = R32(A(3)); e.data.u64 = (uint64_t)R32(A(3) + 8) | ((uint64_t)R32(A(3) + 12) << 32); }
    SYSRET(::epoll_ctl((int)A(0), (int)A(1), (int)A(2), A(3) ? &e : nullptr));
}
IMP(epoll_wait) {
    int n = (int)A(2); std::vector<epoll_event> ev(n > 0 ? n : 1);
    int r = ::epoll_wait((int)A(0), ev.data(), n, (int)A(3));
    if (r < 0) { herr(); RET(-1); return; }
    for (int k = 0; k < r; k++) { uint32_t g = A(1) + 16 * k; W32(g, ev[k].events); W32(g + 4, 0); W32(g + 8, (uint32_t)ev[k].data.u64); W32(g + 12, (uint32_t)(ev[k].data.u64 >> 32)); }
    RET(r);
}
IMP(inet_addr) { RET(::inet_addr(GSTR(A(0)))); }
IMP(inet_aton) { in_addr a; int r = ::inet_aton(GSTR(A(0)), &a); if (r) W32(A(1), a.s_addr); RET(r); }
IMP(inet_ntoa) { static thread_local uint32_t b; if (!b) b = gstatic(32); in_addr a; a.s_addr = A(0); strcpy((char *)GPTR(b), ::inet_ntoa(a)); RET(b); }
IMP(inet_pton) { RET(::inet_pton((int)A(0), GSTR(A(1)), GPTR(A(2)))); }
IMP(inet_ntop) { const char *r = ::inet_ntop((int)A(0), GPTR(A(1)), (char *)GPTR(A(2)), A(3)); RET(r ? A(2) : 0); }
// name resolution: the game's servers are long gone; fail fast instead of waiting for DNS
IMP(getaddrinfo) {
    const char *node = A(0) ? GSTR(A(0)) : "127.0.0.1";
    in_addr ip;
    if (!strcmp(node, "localhost")) node = "127.0.0.1";
    if (!::inet_aton(node, &ip)) { if (verbose) logf("offline: no DNS for %s", node); RET(EAI_NONAME); return; }
    int port = A(1) ? atoi(GSTR(A(1))) : 0;
    uint32_t g = gcalloc(1, 48);    // bionic addrinfo: flags family socktype protocol addrlen canonname addr next
    W32(g + 4, 2); W32(g + 8, A(2) ? R32(A(2) + 8) : 1); W32(g + 12, A(2) ? R32(A(2) + 12) : 6); W32(g + 16, 16); W32(g + 24, g + 32);
    uint8_t sa[16] = {2, 0, (uint8_t)(port >> 8), (uint8_t)port};
    memcpy(sa + 4, &ip.s_addr, 4);
    memcpy(GPTR(g + 32), sa, 16);
    W32(A(3), g); RET(0);
}
IMP(freeaddrinfo) { for (uint32_t g = A(0); g;) { uint32_t n = R32(g + 28); gfree(g); g = n; } }
IMP(gai_strerror) { static uint32_t s; if (!s) s = gstrdup("Name or service not known"); RET(s); }
static uint32_t h_errno_addr;
IMP(__get_h_errno) { if (!h_errno_addr) h_errno_addr = gstatic(4); W32(h_errno_addr, 1); RET(h_errno_addr); }
IMP(gethostbyname) { if (verbose) logf("offline: gethostbyname(%s)", GSTR(A(0))); RET(0); }
IMP(gethostbyaddr) { RET(0); }
IMP(gethostname) { const char *h = "localhost"; strncpy((char *)GPTR(A(0)), h, A(1)); RET(0); }

// ======================================================================= memory mapping
namespace {
struct Map { uint32_t raw, len; };
std::mutex map_mu;
std::map<uint32_t, Map> maps;    // aligned start -> allocation
uint32_t do_map(uint32_t len) {
    uint32_t raw = gmalloc(len + 4096);
    if (!raw) return 0;
    uint32_t al = (raw + 4095) & ~4095u;
    memset(GPTR(al), 0, len);
    std::lock_guard<std::mutex> l(map_mu); maps[al] = Map{raw, len};
    return al;
}
}
IMP(mmap) {
    uint32_t addr = A(0), len = A(1), flags = A(3), off = A(5); int fd = (int)A(4);
    if (flags & 0x10) {                                       // MAP_FIXED: only inside our own mappings
        std::lock_guard<std::mutex> l(map_mu);
        auto it = maps.upper_bound(addr);
        if (it != maps.begin()) { --it; if (addr >= it->first && addr + len <= it->first + it->second.len) {
            if (flags & 0x20) memset(GPTR(addr), 0, len);
            else { ssize_t n = ::pread(fd, GPTR(addr), len, off); (void)n; }
            RET(addr); return; } }
        logf("mmap MAP_FIXED at 0x%08x outside known mappings", addr); gerr(22); RET(0xffffffffu); return;
    }
    uint32_t p = do_map(len);
    if (!p) { gerr(12); RET(0xffffffffu); return; }
    if (!(flags & 0x20) && fd >= 0) {
        ssize_t n = ::pread(fd, GPTR(p), len, off);
        if (n < 0) { herr(); RET(0xffffffffu); return; }
    }
    RET(p);
}
IMP(munmap) {
    std::lock_guard<std::mutex> l(map_mu);
    auto it = maps.find(A(0));
    if (it != maps.end() && A(1) >= it->second.len) { gfree(it->second.raw); maps.erase(it); }
    RET(0);
}
IMP(mremap) {
    uint32_t old = A(0), olen = A(1), nlen = A(2);
    uint32_t p = do_map(nlen); if (!p) { gerr(12); RET(0xffffffffu); return; }
    memcpy(GPTR(p), GPTR(old), std::min(olen, nlen));
    { std::lock_guard<std::mutex> l(map_mu); auto it = maps.find(old); if (it != maps.end()) { gfree(it->second.raw); maps.erase(it); } }
    RET(p);
}
IMP(mprotect) { RET(0); }
IMP(madvise) { RET(0); }
IMP(msync) { RET(0); }
IMP(mlock) { RET(0); }

std::string proc_maps_text() {
    // just enough for stack-bounds lookups: the guest libraries and the current thread's stack
    char line[256]; std::string s;
    Thread *t = self_thread();
    snprintf(line, sizeof line, "%08x-%08x r-xp 00000000 00:00 0 /data/app/lib/libunity.so\n", UNITY_BASE, UNITY_BASE + 0x6c0000); s += line;
    snprintf(line, sizeof line, "%08x-%08x r-xp 00000000 00:00 0 /data/app/lib/libmono.so\n", MONO_BASE, MONO_BASE + 0x3d0000); s += line;
    snprintf(line, sizeof line, "%08x-%08x rw-p 00000000 00:00 0 [stack]\n", t->stack_lo, t->stack_hi); s += line;
    return s;
}

// ======================================================================= dynamic loading
namespace { std::string dl_error; }
IMP(dlopen) {
    const char *n = A(0) ? GSTR(A(0)) : "";
    std::string s = n;
    if (verbose) logf("dlopen(%s)", n);
    // every "library" resolves through the same global namespace: guest exports + runtime imports
    if (s.empty() || s.find("libc") != std::string::npos || s.find("libm.") != std::string::npos || s.find("libdl") != std::string::npos ||
        s.find("liblog") != std::string::npos || s.find("unity") != std::string::npos || s.find("mono") != std::string::npos ||
        s.find("GLES") != std::string::npos || s.find("EGL") != std::string::npos || s.find("libz") != std::string::npos ||
        s.find("__Internal") != std::string::npos || s.find("libandroid") != std::string::npos) { RET(0x0D0D0001u); return; }
    dl_error = "dlopen failed: library \"" + s + "\" not found";
    RET(0);
}
IMP(dlclose) { RET(0); }
IMP(dlsym) {
    const char *n = GSTR(A(1));
    uint32_t t = sym_addr(n);
    if (!t) t = import_trap(n);
    if (!t) { if (verbose) logf("dlsym(%s) not found", n); dl_error = std::string("undefined symbol: ") + n; }
    RET(t);
}
IMP(dlerror) {
    static uint32_t buf; if (!buf) buf = gstatic(512);
    if (dl_error.empty()) { RET(0); return; }
    strncpy((char *)GPTR(buf), dl_error.c_str(), 511); dl_error.clear(); RET(buf);
}

// ======================================================================= environment / process / users
namespace {
std::mutex env_mu;
std::map<std::string, std::string> env_vars;
uint32_t env_array;
void rebuild_env() {
    uint32_t arr = gstatic(4 * (env_vars.size() + 1));
    int k = 0;
    for (auto &kv : env_vars) W32(arr + 4 * k++, gstrdup((kv.first + "=" + kv.second).c_str()));
    if (env_array) W32(env_array, arr);
}
}
extern "C" uint32_t rt_environ() {
    if (!env_array) { env_array = gstatic(4); rebuild_env(); }
    return env_array;
}
void env_set(const std::string &k, const std::string &v) { std::lock_guard<std::mutex> g(env_mu); env_vars[k] = v; rebuild_env(); }
IMP(getenv) {
    std::lock_guard<std::mutex> g(env_mu);
    static std::map<std::string, uint32_t> cache;
    auto it = env_vars.find(GSTR(A(0)));
    if (it == env_vars.end()) { RET(0); return; }
    uint32_t &p = cache[it->first + "=" + it->second];
    if (!p) p = gstrdup(it->second.c_str());
    RET(p);
}
IMP(setenv) {
    std::lock_guard<std::mutex> g(env_mu);
    std::string k = GSTR(A(0));
    if (!A(2) && env_vars.count(k)) { RET(0); return; }
    env_vars[k] = GSTR(A(1)); rebuild_env(); RET(0);
}
IMP(unsetenv) { std::lock_guard<std::mutex> g(env_mu); env_vars.erase(GSTR(A(0))); rebuild_env(); RET(0); }
IMP(getuid) { RET(10123); }
IMP(geteuid) { RET(10123); }
IMP(getresuid) { W32(A(0), 10123); W32(A(1), 10123); W32(A(2), 10123); RET(0); }
IMP(setresuid) { RET(0); }
static uint32_t make_passwd() {
    static uint32_t pw;
    if (!pw) {
        pw = gstatic(32);
        W32(pw, gstrdup("u0_a123")); W32(pw + 4, gstrdup("")); W32(pw + 8, 10123); W32(pw + 12, 10123);
        W32(pw + 16, gstrdup(data_dir.c_str())); W32(pw + 20, gstrdup("/system/bin/sh"));
    }
    return pw;
}
IMP(getpwuid) { RET(make_passwd()); }
IMP(getpwnam) { RET(make_passwd()); }
IMP(getpwuid_r) { RET(2); }
IMP(getgrgid) { RET(0); }
IMP(getgrnam) { RET(0); }
IMP(uname) {
    uint32_t u = A(0); memset(GPTR(u), 0, 65 * 6);
    const char *f[5] = {"Linux", "localhost", "3.4.0", "#1 SMP PREEMPT", "armv7l"};
    for (int k = 0; k < 5; k++) strcpy((char *)GPTR(u + 65 * k), f[k]);
    RET(0);
}
IMP(getrusage) {
    memset(GPTR(A(1)), 0, 72);
    struct rusage r{}; ::getrusage(RUSAGE_SELF, &r);
    W32(A(1), (uint32_t)r.ru_utime.tv_sec); W32(A(1) + 4, (uint32_t)r.ru_utime.tv_usec);
    W32(A(1) + 8, (uint32_t)r.ru_stime.tv_sec); W32(A(1) + 12, (uint32_t)r.ru_stime.tv_usec);
    RET(0);
}
IMP(sysconf) {
    int n = (int)A(0);
    switch (n) {
    case 0x27: case 0x28: RET(4096); return;               // _SC_PAGESIZE / _SC_PAGE_SIZE
    case 0x60: case 0x61: RET(4); return;                  // _SC_NPROCESSORS_CONF / ONLN
    case 0x62: RET(512 * 1024); return;                    // _SC_PHYS_PAGES
    case 0x6: RET(100); return;                            // _SC_CLK_TCK
    case 0x7: RET(1024); return;                           // _SC_OPEN_MAX
    }
    RET((uint32_t)-1);
}
IMP(prctl) { RET(0); }
IMP(fork) { gerr(38); RET(-1); }
IMP(execl) { gerr(38); RET(-1); }
IMP(execv) { gerr(38); RET(-1); }
IMP(execve) { gerr(38); RET(-1); }
IMP(waitpid) { gerr(10); RET(-1); }
IMP(system) { RET(-1); }
IMP(setitimer) { RET(0); }
IMP(__errno) { self_thread(); RET(G32(guest_errno)); }
// bionic: __pthread_cleanup_t { prev, routine, arg }
IMP(__pthread_cleanup_push) { W32(A(0), 0); W32(A(0) + 4, A(1)); W32(A(0) + 8, A(2)); }
IMP(__pthread_cleanup_pop) { if (A(1) && R32(A(0) + 4)) call_c(c, R32(A(0) + 4), {R32(A(0) + 8)}); }

// ======================================================================= zlib (32-bit z_stream <-> host z_stream)
static std::mutex z_mu;
static std::unordered_map<uint32_t, z_stream *> zs;
static z_stream *zin(uint32_t g) {
    std::lock_guard<std::mutex> l(z_mu); z_stream *&s = zs[g]; if (!s) s = (z_stream *)calloc(1, sizeof(z_stream));
    s->next_in = (Bytef *)GPTR(R32(g)); s->avail_in = R32(g + 4); s->next_out = (Bytef *)GPTR(R32(g + 12)); s->avail_out = R32(g + 16);
    return s;
}
static void zout(uint32_t g, z_stream *s) {
    W32(g, G32(s->next_in)); W32(g + 4, s->avail_in); W32(g + 8, (uint32_t)s->total_in);
    W32(g + 12, G32(s->next_out)); W32(g + 16, s->avail_out); W32(g + 20, (uint32_t)s->total_out);
    W32(g + 24, 0); W32(g + 44, s->data_type); W32(g + 48, (uint32_t)s->adler);
}
static void zfree(uint32_t g) { std::lock_guard<std::mutex> l(z_mu); auto it = zs.find(g); if (it != zs.end()) { free(it->second); zs.erase(it); } }
IMP(inflateInit_) { z_stream *s = zin(A(0)); int r = inflateInit(s); zout(A(0), s); RET(r); }
IMP(inflateInit2_) { z_stream *s = zin(A(0)); int r = inflateInit2(s, (int)A(1)); zout(A(0), s); RET(r); }
IMP(inflate) { z_stream *s = zin(A(0)); int r = inflate(s, (int)A(1)); zout(A(0), s); RET(r); }
IMP(inflateReset) { z_stream *s = zin(A(0)); int r = inflateReset(s); zout(A(0), s); RET(r); }
IMP(inflateEnd) { z_stream *s = zin(A(0)); int r = inflateEnd(s); zout(A(0), s); zfree(A(0)); RET(r); }
IMP(deflateInit2_) { z_stream *s = zin(A(0)); int r = deflateInit2(s, (int)A(1), (int)A(2), (int)A(3), (int)A(4), (int)A(5)); zout(A(0), s); RET(r); }
IMP(deflateInit_) { z_stream *s = zin(A(0)); int r = deflateInit(s, (int)A(1)); zout(A(0), s); RET(r); }
IMP(deflate) { z_stream *s = zin(A(0)); int r = deflate(s, (int)A(1)); zout(A(0), s); RET(r); }
IMP(deflateReset) { z_stream *s = zin(A(0)); int r = deflateReset(s); zout(A(0), s); RET(r); }
IMP(deflateEnd) { z_stream *s = zin(A(0)); int r = deflateEnd(s); zout(A(0), s); zfree(A(0)); RET(r); }
IMP(crc32) { RET(crc32(A(0), (const Bytef *)GPTR(A(1)), A(2))); }
