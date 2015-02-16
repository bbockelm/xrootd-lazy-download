
#include <stdio.h>
#include <sys/mman.h>

#include <algorithm>

#include <XrdVersion.hh>
#include <XrdSys/XrdSysPthread.hh>

#include "XrdClLazyDownload.hh"
#include "LocalFileSystem.hh"

using namespace XrdClLazyDownload;


XrdVERSIONINFO(XrdClGetPlugIn, XrdClLazyDownload);

LocalFileSystem g_lfs;
XrdSysMutex g_mutex;


LDFile::LDFile(const std::string &cache_dir)
:
  m_is_open(false)
, m_count(0)
, m_fd(-1)
, m_size(-1)
, m_cacheSize(-1)
, m_fh(false) // disable plugins
, m_cache_dir(cache_dir)
{
    std::string pattern(cache_dir);
    if (pattern.empty())
    {
        if (char *p = getenv("TMPDIR"))
        {
            pattern = p;
        }
    }
    if (pattern.empty()) {pattern = "/tmp";}

    pattern += "/xrootd-shadow-XXXXXX";
    std::vector<char> temp(pattern.c_str(), pattern.c_str()+pattern.size()+1);
    m_fd = mkstemp(&temp[0]);
    if (m_fd != -1) {unlink(&temp[0]);}
}


void
LDFile::OpenResponseHandler::HandleResponseWithHosts(XrdCl::XRootDStatus *status,
                                                     XrdCl::AnyObject    *response,
                                                     XrdCl::HostList     *hostList)
{
    if (status && status->IsOK())
    {
        XrdCl::StatInfo *sinfo = NULL;
        if (m_parent.m_fh.Stat(false, sinfo, 0).IsOK())
        {
            m_parent.SetSize(sinfo->GetSize());
        }
    }
    if (m_handler) {m_handler->HandleResponseWithHosts(status, response, hostList);}

    delete this;
}


void
LDFile::SetSize(uint64_t size)
{
    m_is_open = true;
    m_size = size;
    m_cacheSize = (m_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    m_present.resize(m_cacheSize, false);
    if (m_fd != -1)
    {
        if (-1 == ftruncate(m_fd, size))
        {
            close(m_fd);
            m_fd = -1;
        }
    }
}


XrdCl::XRootDStatus
LDFile::Open(const std::string        &url,
             XrdCl::OpenFlags::Flags   flags,
             XrdCl::Access::Mode       mode,
             XrdCl::ResponseHandler   *handler,
             uint16_t                  timeout)
{
    XrdCl::ResponseHandler *file_handler = static_cast<XrdCl::ResponseHandler*>(new OpenResponseHandler(*this, handler));

    return m_fh.Open(url, flags, mode, file_handler, timeout);
}


XrdCl::XRootDStatus
LDFile::Close(XrdCl::ResponseHandler *handler,
              uint16_t                timeout)
{
    m_is_open = false;
    return m_fh.Close(handler, timeout);
}


XrdCl::XRootDStatus
LDFile::Stat(bool                    force,
             XrdCl::ResponseHandler *handler,
             uint16_t                timeout)
{
    return m_fh.Stat(force, handler, timeout);
}


XrdCl::XRootDStatus
LDFile::Read(uint64_t                offset,
             uint32_t                size,
             void                   *buffer,
             XrdCl::ResponseHandler *handler,
             uint16_t                timeout)
{
    fprintf(stderr, "Starting read\n");
    if (!UseCache())
    {
        fprintf(stderr, "Not using cache.\n");
        return m_fh.Read(offset, size, buffer, handler, timeout);
    }
    XrdCl::XRootDStatus status = cache(offset, offset+size, timeout);
    fprintf(stderr, "Finished cache.\n");
    if (!status.IsOK()) {return status;}

    int64_t nread = 0, nread_tmp = -1, len_tmp = size, start_tmp = offset;
    while ((len_tmp > 0) && (nread_tmp != 0))
    {
        while (((nread_tmp = pread(m_fd, static_cast<char*>(buffer) + nread, len_tmp, start_tmp)) < 0) && (errno == EAGAIN || errno == EINTR)) {}
        if (nread_tmp == -1)
        {
            return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOSError, errno);
        }
        nread += nread_tmp;
        len_tmp -= nread_tmp;
        start_tmp += nread;
    }
    XrdCl::XRootDStatus *cbstatus = new XrdCl::XRootDStatus(XrdCl::stOK, XrdCl::suDone);
    XrdCl::AnyObject *response = new XrdCl::AnyObject();
    XrdCl::ChunkInfo *info = new XrdCl::ChunkInfo(offset, size, buffer);
    response->Set(info);
    fprintf(stderr, "Calling handler.\n");
    handler->HandleResponseWithHosts(cbstatus, response, NULL);
    return XrdCl::XRootDStatus(XrdCl::stOK, XrdCl::suDone);
}


XrdCl::XRootDStatus
LDFile::Write(uint64_t                offset,
      uint32_t                size,
      const void             *buffer,
      XrdCl::ResponseHandler *handler,
      uint16_t                timeout)
{
    (void)offset; (void)size; (void)buffer; (void)handler; (void)timeout;
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported);
}


XrdCl::XRootDStatus
LDFile::Sync(XrdCl::ResponseHandler *handler,
             uint16_t                timeout)
{
    return m_fh.Sync(handler, timeout);
}


XrdCl::XRootDStatus
LDFile::Truncate(uint64_t                size,
                 XrdCl::ResponseHandler *handler,
                 uint16_t                timeout)
{
    (void)size; (void) handler; (void) timeout;
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported);
}


XrdCl::XRootDStatus
LDFile::VectorRead(const XrdCl::ChunkList &chunks,
           void                   *buffer,
           XrdCl::ResponseHandler *handler,
           uint16_t                timeout)
{
    if (!UseCache())
    {
        return m_fh.VectorRead(chunks, buffer, handler, timeout);
    }
    XrdCl::ChunkList::const_iterator it;
    int64_t offset = 0;
    for (it=chunks.begin(); it!=chunks.end(); it++)
    {
        void *chunk_buffer = it->buffer ? it->buffer : (static_cast<char*>(buffer)+offset);
        offset += it->length;
        XrdCl::XRootDStatus status = Read(it->offset, it->length, chunk_buffer, NULL, timeout);
        if (!status.IsOK()) {return status;}
    }
    XrdCl::XRootDStatus *cbstatus = new XrdCl::XRootDStatus(XrdCl::stOK, XrdCl::suDone);
    XrdCl::AnyObject *response = new XrdCl::AnyObject();
    XrdCl::ChunkList *chunks_copy = new XrdCl::ChunkList(chunks);
    response->Set(chunks_copy);
    handler->HandleResponseWithHosts(cbstatus, response, NULL);
    // TODO: figure out the right way to call back the handler.
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotImplemented);
}


XrdCl::XRootDStatus
LDFile::Fcntl(const XrdCl::Buffer    &arg,
      XrdCl::ResponseHandler *handler,
      uint16_t                timeout)
{
    return m_fh.Fcntl(arg, handler, timeout);
}


XrdCl::XRootDStatus
LDFile::Visa(XrdCl::ResponseHandler *handler,
             uint16_t                timeout)
{
    return m_fh.Visa(handler, timeout);
}


bool
LDFile::IsOpen() const
{
    return m_is_open;
}


bool
LDFile::UseCache() const
{
    return ((m_fd != -1) && (m_cacheSize != -1));
}

bool
LDFile::SetProperty(const std::string &name,
                    const std::string &value)
{
    return m_fh.SetProperty(name, value);
}


bool
LDFile::GetProperty(const std::string &name,
                    std::string &value) const
{
    return m_fh.GetProperty(name, value);
}


XrdCl::XRootDStatus
LDFile::cache(uint64_t start, uint64_t end, uint16_t timeout)
{
    if (!UseCache()) {return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInvalidOp, 0, "Non-cached file");}

    start = (start / CHUNK_SIZE) * CHUNK_SIZE;
    end = (end < static_cast<uint64_t>(m_size)) ? end : m_size;

    int64_t index = start / CHUNK_SIZE;

    while (start < end)
    {
        int64_t len = (m_size - start < static_cast<int64_t>(CHUNK_SIZE)) ? (m_size - start) : CHUNK_SIZE;
        if (start + len > static_cast<uint64_t>(m_size))
        {
            len = m_size - start;
        }
        if (!m_present[index])
        {
            void *window = mmap(0, len, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, start);
            if (window == MAP_FAILED)
            {
                return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errOSError, errno);
            }

            uint32_t bytesRead = 0;
            XrdCl::XRootDStatus status;
            uint64_t len_tmp = len, start_tmp = start, nread_tmp = 0;
            fprintf(stderr, "Issuing blocking cache read.\n");
            while ((status = m_fh.Read(start_tmp, len_tmp, (static_cast<char*>(window)+nread_tmp), bytesRead, timeout)).IsOK())
            {
                fprintf(stderr, "Finished read of %d bytes.\n", bytesRead);
                if (bytesRead == 0)
                {
                    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errInvalidOp, 1, "Read past end of file.");
                    munmap(window, len);
                }
                len_tmp -= bytesRead;
                start_tmp += bytesRead;
                nread_tmp += bytesRead;
	        if (len_tmp == 0) {break;}
            }
            munmap(window, len);
            if (!status.IsOK()) {return status;}

            m_present[index] = 1;
            m_count++;
            if (m_count == m_cacheSize)
            {
                m_fh.Close();;
            }
        }
        start += len;
        ++index;
    }
    return XrdCl::XRootDStatus(XrdCl::stOK, XrdCl::suDone);
}


LDFileSystem::LDFileSystem(const XrdCl::URL &url)
:
m_fs(url, false) // disable plugins
{}


XrdCl::XRootDStatus
LDFileSystem::Locate(const std::string        &path,
                     XrdCl::OpenFlags::Flags   flags,
                     XrdCl::ResponseHandler   *handler,
                     uint16_t                  timeout)
{
    return m_fs.Locate(path, flags, handler, timeout);
}


XrdCl::XRootDStatus
LDFileSystem::Mv(const std::string        &source,
                 const std::string        &dest,
                 XrdCl::ResponseHandler   *handler,
                 uint16_t                  timeout)
{
    (void)source; (void)dest; (void)handler; (void)timeout;
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported);
}


XrdCl::XRootDStatus
LDFileSystem::Query(XrdCl::QueryCode::Code  queryCode,
                    const XrdCl::Buffer    &arg,
                    XrdCl::ResponseHandler *handler,
                    uint16_t               timeout)
{
    return m_fs.Query(queryCode, arg, handler, timeout);
}


XrdCl::XRootDStatus
LDFileSystem::Truncate(const std::string        &path,
                       uint64_t                  size,
                       XrdCl::ResponseHandler   *handler,
                       uint16_t                  timeout)
{
    (void)path; (void)size; (void)handler; (void)timeout;
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported);
}


XrdCl::XRootDStatus
LDFileSystem::Rm(const std::string        &path,
                 XrdCl::ResponseHandler   *handler,
                 uint16_t                  timeout)
{
    (void)path; (void)handler; (void)timeout;
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported);
}


XrdCl::XRootDStatus
LDFileSystem::MkDir(const std::string        &path,
                    XrdCl::MkDirFlags::Flags  flags,
                    XrdCl::Access::Mode       mode,
                    XrdCl::ResponseHandler   *handler,
                    uint16_t                  timeout)
{
    (void)path; (void)flags; (void)mode; (void)handler; (void)timeout;
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported);
}


XrdCl::XRootDStatus
LDFileSystem::RmDir(const std::string        &path,
                    XrdCl::ResponseHandler   *handler,
                    uint16_t                  timeout)
{
    (void)path; (void)handler; (void)timeout;
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported);
}


XrdCl::XRootDStatus
LDFileSystem::ChMod(const std::string        &path,
                    XrdCl::Access::Mode       mode,
                    XrdCl::ResponseHandler   *handler,
                    uint16_t                  timeout)
{
    (void)path; (void)mode; (void)handler; (void)timeout;
    return XrdCl::XRootDStatus(XrdCl::stError, XrdCl::errNotSupported);
}


XrdCl::XRootDStatus
LDFileSystem::Ping(XrdCl::ResponseHandler *handler,
                   uint16_t                timeout)
{
    return m_fs.Ping(handler, timeout);
}


XrdCl::XRootDStatus
LDFileSystem::Stat(const std::string        &path,
                   XrdCl::ResponseHandler   *handler,
                   uint16_t                  timeout)
{
    return m_fs.Stat(path, handler, timeout);
}


XrdCl::XRootDStatus
LDFileSystem::StatVFS(const std::string        &path,
                      XrdCl::ResponseHandler   *handler,
                      uint16_t                  timeout)
{
    return m_fs.StatVFS(path, handler, timeout);
}


XrdCl::XRootDStatus
LDFileSystem::Protocol(XrdCl::ResponseHandler *handler,
                       uint16_t                timeout)
{
    return m_fs.Protocol(handler, timeout);
}


XrdCl::XRootDStatus
LDFileSystem::DirList(const std::string          &path,
                      XrdCl::DirListFlags::Flags  flags,
                      XrdCl::ResponseHandler     *handler,
                      uint16_t                    timeout)
{
    return m_fs.DirList(path, flags, handler, timeout);
}


XrdCl::XRootDStatus
LDFileSystem::SendInfo(const std::string        &info,
                       XrdCl::ResponseHandler   *handler,
                       uint16_t                  timeout)
{
    return m_fs.SendInfo(info, handler, timeout);
}


XrdCl::XRootDStatus
LDFileSystem::Prepare(const std::vector<std::string>        &fileList,
                      XrdCl::PrepareFlags::Flags             flags,
                      uint8_t                                priority,
                      XrdCl::ResponseHandler                *handler,
                      uint16_t                               timeout)
{
    return m_fs.Prepare(fileList, flags, priority, handler, timeout);
}


bool
LDFileSystem::SetProperty(const std::string &name,
                          const std::string &value)
{
    return m_fs.SetProperty(name, value);
}


bool
LDFileSystem::GetProperty(const std::string &name,
                          std::string &value ) const
{
    return m_fs.GetProperty(name, value);
}


PlugInFactory::PlugInFactory(const std::map<std::string, std::string> &args)
:
m_min_free(2.0)
{
    m_dirs.reserve(2);
    m_dirs.push_back(".");
    m_dirs.push_back("$TMPDIR");

    std::map<std::string, std::string>::const_iterator it;
    if ((it = args.find("tempDir")) != args.end())
    {
        ssize_t begin = 0;
        const std::string &s = it->second;
        m_dirs.clear();
        m_dirs.reserve(std::count(s.begin(), s.end(), ':') + 1);

        while (true)
        {
            size_t end = s.find(':', begin);
            if (end == std::string::npos)
            {
                m_dirs.push_back(s.substr(begin, end));
                break;
            }
            else
            {
                m_dirs.push_back(s.substr(begin, end - begin));
                begin = end+1;
            }
        }
    }
}


XrdCl::FilePlugIn *
PlugInFactory::CreateFile(const std::string & url)
{
    fprintf(stderr, "Creating new path for url %s.\n", url.c_str());
    std::string temp_path;
    {
        XrdSysMutexHelper scopedLock(g_mutex);
        g_lfs.findCachePath(m_dirs, m_min_free);
    }
    return static_cast<XrdCl::FilePlugIn *>(new LDFile(temp_path));
}


XrdCl::FileSystemPlugIn *
PlugInFactory::CreateFileSystem(const std::string &url_str)
{
    fprintf(stderr, "Creating new filesystem for url %s.\n", url_str.c_str());
    XrdCl::URL url(url_str);
    return static_cast<XrdCl::FileSystemPlugIn *>(new LDFileSystem(url));
}


extern "C"
{
void *XrdClGetPlugIn(const void *arg)
{
    fprintf(stderr, "Returning new plugin factory.\n");
    const std::map<std::string, std::string> &args = *static_cast<const std::map<std::string, std::string> *>(arg);
    void * myplugin = new XrdClLazyDownload::PlugInFactory(args);
    fprintf(stderr, "New factory plugin at %p.\n", myplugin);

    return myplugin;
}
}


