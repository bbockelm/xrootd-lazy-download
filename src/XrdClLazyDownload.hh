#ifndef XRD_CL_LAZY_DOWNLOAD_H
#define XRD_CL_LAZY_DOWNLOAD_H

#include <XrdSys/XrdSysPthread.hh>
#include <XrdCl/XrdClPlugInInterface.hh>

namespace XrdClLazyDownload
{

class LDFile : public XrdCl::FilePlugIn
{

public:

    LDFile(const std::string &cache_dir);

    virtual ~LDFile() {}

    virtual XrdCl::XRootDStatus Open( const std::string        &url,
                                      XrdCl::OpenFlags::Flags   flags,
                                      XrdCl::Access::Mode       mode,
                                      XrdCl::ResponseHandler   *handler,
                                      uint16_t                  timeout );

    virtual XrdCl::XRootDStatus Close( XrdCl::ResponseHandler *handler,
                                       uint16_t                timeout );

    virtual XrdCl::XRootDStatus Stat( bool                    force,
                                      XrdCl::ResponseHandler *handler,
                                      uint16_t                timeout );

    virtual XrdCl::XRootDStatus Read( uint64_t                offset,
                                      uint32_t                size,
                                      void                   *buffer,
                                      XrdCl::ResponseHandler *handler,
                                      uint16_t                timeout );

    // Not actually implemented - writing will invalidate our cache.
    virtual XrdCl::XRootDStatus Write( uint64_t                offset,
                                       uint32_t                size,
                                       const void             *buffer,
                                       XrdCl::ResponseHandler *handler,
                                       uint16_t                timeout );

    virtual XrdCl::XRootDStatus Sync( XrdCl::ResponseHandler *handler,
                                      uint16_t                timeout );

    virtual XrdCl::XRootDStatus Truncate( uint64_t                size,
                                          XrdCl::ResponseHandler *handler,
                                          uint16_t                timeout );

    virtual XrdCl::XRootDStatus VectorRead( const XrdCl::ChunkList &chunks,
                                            void                   *buffer,
                                            XrdCl::ResponseHandler *handler,
                                            uint16_t                timeout );

    virtual XrdCl::XRootDStatus Fcntl( const XrdCl::Buffer    &arg,
                                       XrdCl::ResponseHandler *handler,
                                       uint16_t                timeout );

    virtual XrdCl::XRootDStatus Visa( XrdCl::ResponseHandler *handler,
                                      uint16_t                timeout );

    virtual bool IsOpen() const;

    virtual bool SetProperty( const std::string &name,
                              const std::string &value );

    virtual bool GetProperty( const std::string &name,
                              std::string &value ) const;

private:

    class OpenResponseHandler : public XrdCl::ResponseHandler
    {
    public:

        OpenResponseHandler(LDFile &parent, XrdCl::ResponseHandler *handler)
          : m_parent(parent), m_handler(handler)
        {}

        virtual ~OpenResponseHandler() {}

        virtual void HandleResponseWithHosts( XrdCl::XRootDStatus *status,
                                              XrdCl::AnyObject    *response,
                                              XrdCl::HostList     *hostList );

    private:

        LDFile &m_parent;
        XrdCl::ResponseHandler *m_handler;
    };

    void SetSize(uint64_t);
    XrdCl::XRootDStatus cache(uint64_t start, uint64_t end, uint16_t timeout);
    bool UseCache() const;

    static const uint64_t CHUNK_SIZE = 64*1024*1024;

    bool m_is_open;
    unsigned m_count;
    int m_fd;
    int64_t m_size;
    int64_t m_cacheSize;
    XrdCl::File m_fh;
    const std::string m_cache_dir;
    std::vector<bool> m_present;
    XrdSysMutex m_mutex;
};


class LDFileSystem : public XrdCl::FileSystemPlugIn
{
public:

    LDFileSystem(const XrdCl::URL &url);

    virtual ~LDFileSystem() {}

    virtual XrdCl::XRootDStatus Locate( const std::string        &path,
                                        XrdCl::OpenFlags::Flags   flags,
                                        XrdCl::ResponseHandler   *handler,
                                        uint16_t                  timeout );

    virtual XrdCl::XRootDStatus Mv( const std::string        &source,
                                    const std::string        &dest,
                                    XrdCl::ResponseHandler   *handler,
                                    uint16_t                  timeout );

    virtual XrdCl::XRootDStatus Query( XrdCl::QueryCode::Code  queryCode,
                                       const XrdCl::Buffer    &arg,
                                       XrdCl::ResponseHandler *handler,
                                       uint16_t               timeout );

    virtual XrdCl::XRootDStatus Truncate( const std::string        &path,
                                          uint64_t                  size,
                                          XrdCl::ResponseHandler   *handler,
                                          uint16_t                  timeout );

    virtual XrdCl::XRootDStatus Rm( const std::string        &path,
                                    XrdCl::ResponseHandler   *handler,
                                    uint16_t                  timeout );

    virtual XrdCl::XRootDStatus MkDir( const std::string        &path,
                                       XrdCl::MkDirFlags::Flags  flags,
                                       XrdCl::Access::Mode       mode,
                                       XrdCl::ResponseHandler   *handler,
                                       uint16_t                  timeout );

    virtual XrdCl::XRootDStatus RmDir( const std::string        &path,
                                       XrdCl::ResponseHandler   *handler,
                                       uint16_t                  timeout );

    virtual XrdCl::XRootDStatus ChMod( const std::string        &path,
                                       XrdCl::Access::Mode       mode,
                                       XrdCl::ResponseHandler   *handler,
                                       uint16_t                  timeout );

    virtual XrdCl::XRootDStatus Ping( XrdCl::ResponseHandler *handler,
                                      uint16_t                timeout );

    virtual XrdCl::XRootDStatus Stat( const std::string        &path,
                                      XrdCl::ResponseHandler   *handler,
                                      uint16_t                  timeout );

    virtual XrdCl::XRootDStatus StatVFS( const std::string        &path,
                                         XrdCl::ResponseHandler   *handler,
                                         uint16_t                  timeout );

    virtual XrdCl::XRootDStatus Protocol( XrdCl::ResponseHandler *handler,
                                          uint16_t                timeout = 0 );

    virtual XrdCl::XRootDStatus DirList( const std::string          &path,
                                         XrdCl::DirListFlags::Flags  flags,
                                         XrdCl::ResponseHandler     *handler,
                                         uint16_t                    timeout );

    virtual XrdCl::XRootDStatus SendInfo( const std::string        &info,
                                          XrdCl::ResponseHandler   *handler,
                                          uint16_t                  timeout );

    virtual XrdCl::XRootDStatus Prepare( const std::vector<std::string>        &fileList,
                                         XrdCl::PrepareFlags::Flags             flags,
                                         uint8_t                                priority,
                                         XrdCl::ResponseHandler                *handler,
                                         uint16_t                               timeout );

    virtual bool SetProperty( const std::string &name,
                              const std::string &value );

    virtual bool GetProperty( const std::string &name,
                              std::string &value ) const;

private:
    XrdCl::FileSystem m_fs;
};


class PlugInFactory : public XrdCl::PlugInFactory
{
public:

    PlugInFactory(const std::map<std::string, std::string> &args);

    virtual ~PlugInFactory() {fprintf(stderr, "Deleting plugin factory.\n");}

    virtual XrdCl::FilePlugIn *CreateFile( const std::string &url );

    virtual XrdCl::FileSystemPlugIn *CreateFileSystem( const std::string &url );

private:

    double m_min_free;
    std::vector<std::string> m_dirs;
    std::string m_temp_path;
};

};
#endif
