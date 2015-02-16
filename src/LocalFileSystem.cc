
/*
 *  This file is part of CMSSW and has been adapted for XrdCl
 *
 *  Copyright (c) 2008-2015, CERN..
 *
 *  LocalFileSystem is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  LocalFileSystem is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with LocalFileSystem.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE 1
#define _FILE_OFFSET_BITS 64
#include "LocalFileSystem.hh"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/param.h>
#if BSD
# include <sys/statvfs.h>
# include <sys/ucred.h>
# include <sys/mount.h>
#else
# include <mntent.h>
# include <sys/vfs.h>
#endif
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

#pragma GCC diagnostic ignored "-Wformat" // shut warning on '%z'


/// Information about file systems on this node.
struct LocalFileSystem::FSInfo
{
  char		*fsname;	//< file system name
  char		*type;		//< file system type
  char		*dir;		//< mount point directory
  dev_t		dev;		//< device id
  long		fstype;		//< file system id
  double	freespc;	//< free space in megabytes
  unsigned 	local : 1;	//< flag for local device
  unsigned	checked : 1;	//< flag for valid dev, fstype
};


/** Read /proc/filesystems to determine which filesystems are local,
    meaning access latency is tolerably small, and operating system
    buffer cache will likely do a good job at caching file contents
    and accelerate many small file operations reasonably well.

    The /proc list enumerates all filesystems known by the kernel,
    except a few special ones like /dev and /selinux. The ones marked
    as "nodev" have unstable device definition, meaning they are some
    way or another "virtual" file systems.  This labeling is used by
    kernel nfsd to determine which file systems are safe for exporting
    without help (fixing fsid), and turns out to be close enough to
    list of file systems that we can consider to be high-speed local,
    minus a few exceptions.  Everything else we consider "remote" or
    "slow" file systems where application should prefer massive bulk
    streaming I/O for better performance.

    The exceptions to /proc/filesystems list: lustre and fuse file
    systems are forced to remote status. Everything else like NFS,
    AFS, GPFS and various cluster-based systems are already remote. */
int
LocalFileSystem::readFSTypes(void)
{
  int ret = 0;

#if __linux__
  static const char procfs[] = "/proc/filesystems";
  FILE *fs = fopen(procfs, "r");
  if (! fs)
  {
    return -1;
  }

  ssize_t nread;
  int line = 0;
  while (! feof(fs))
  {
    char *type = 0;
    char *fstype = 0;
    size_t len = 0;
    ++line;

    if ((nread = getdelim(&type, &len, '\t', fs)) == -1 && ! feof(fs))
    {
      fprintf(stderr, "%s:%d: %s (%zd; 1)\n",
	      procfs, line, strerror(errno), nread);
      free(type);
      ret = -1;
      break;
    }

    if ((nread = getdelim(&fstype, &len, '\n', fs)) == -1 && ! feof(fs))
    {
      fprintf(stderr, "%s:%d: %s (%zd; 2)\n",
	      procfs, line, strerror(errno), nread);
      free(type);
      free(fstype);
      ret = -1;
      break;
    }

    if (feof (fs))
    {
      free(type);
      free(fstype);
      break;
    }
    
    if (! strcmp(type, "nodev\t")
	|| ! strcmp(fstype, "lustre\n")
	|| ! strncmp(fstype, "fuse", 4))
    {
      free(type);
      free(fstype);
      continue;
    }

    assert(nread >= 1);
    fstype[nread-1] = 0;
    fstypes_.push_back(fstype);
    free(fstype);
    free(type);
  }

  fclose(fs);
#endif // __linux__

  return ret;
}


/** Initialise file system description from /etc/mtab info.

    This function saves the information from getmntent(), matching the
    file system type to the known local ones.  It only remembers the
    information from /etc/mtab, so the dev and fstype attributes are
    not yet valid; call statFSInfo() to fill those in.  This avoids
    touching irrelevant filesystems unnecessarily; the file system may
    not be fully functional, or partially offline, or just very slow. */
LocalFileSystem::FSInfo *
LocalFileSystem::initFSInfo(void *arg)
{
#if BSD
  struct statfs *m = static_cast<struct statfs *>(arg);
  size_t infolen = sizeof(struct FSInfo);
  size_t fslen = strlen(m->f_mntfromname) + 1;
  size_t dirlen = strlen(m->f_mntonname) + 1;
  size_t typelen = strlen(m->f_fstypename) + 1;
  size_t totlen = infolen + fslen + dirlen + typelen;
  FSInfo *i = (FSInfo *) malloc(totlen);
  char *p = (char *) i;
  i->fsname = strncpy(p += infolen, m->f_mntfromname, fslen);
  i->type = strncpy(p += fslen, m->f_fstypename, typelen);
  i->dir = strncpy(p += typelen, m->f_mntonname, dirlen);
  i->dev = m->f_fsid.val[0];
  i->fstype = m->f_type;
  i->freespc = 0;
  if (m->f_bsize > 0)
  {
    i->freespc = m->f_bavail;
    i->freespc *= m->f_bsize;
    i->freespc /= 1024. * 1024. * 1024.;
  } 
  /* FIXME: This incorrectly says that mounted disk images are local,
     even if it was mounted from a network server. The alternative is
     to walk up the device tree using either a) process IORegistry to
     get the device tree, which lists devices for disk images, and from
     there translate volume uuid to a mount point; b) parse output from
     'hdiutil info -plist' to determine image-path / dev-entry map. */
  i->local = ((m->f_flags & MNT_LOCAL) ? 1 : 0);
  i->checked = 1;
  return i;

#else // ! BSD
  mntent *m = static_cast<mntent *>(arg);
  size_t infolen = sizeof(struct FSInfo);
  size_t fslen = strlen(m->mnt_fsname) + 1;
  size_t dirlen = strlen(m->mnt_dir) + 1;
  size_t typelen = strlen(m->mnt_type) + 1;
  size_t totlen = infolen + fslen + dirlen + typelen;
  FSInfo *i = (FSInfo *) malloc(totlen);
  char *p = (char *) i;
  i->fsname = strncpy(p += infolen, m->mnt_fsname, fslen);
  i->type = strncpy(p += fslen, m->mnt_type, typelen);
  i->dir = strncpy(p += typelen, m->mnt_dir, dirlen);
  i->dev = -1;
  i->fstype = -1;
  i->freespc = 0;
  i->local = 0;
  i->checked = 0;

  for (size_t j = 0; j < fstypes_.size() && ! i->local; ++j)
    if (fstypes_[j] == i->type)
      i->local = 1;
#endif // BSD

  return i;
}


/** Initialise the list of currently mounted file systems.

    Reads /etc/mtab (or equivalent) to determine all currently mounted
    file systems, and initialises FSInfo structure for them.  It does
    not yet call statFSInfo() on them, so the device and file type ids
    are not yet complete. */
int
LocalFileSystem::initFSList(void)
{
#if BSD
  int rc;
  struct statfs *mtab = 0;
  if ((rc = getmntinfo(&mtab, MNT_NOWAIT)) < 0)
  {
    return -1;
  }

  fs_.reserve(rc);
  for (int ix = 0; ix < rc; ++ix)
    fs_.push_back(initFSInfo(&mtab[ix]));

  free(mtab);
#else
  struct mntent *m;
  FILE *mtab = setmntent(_PATH_MOUNTED, "r");
  if (! mtab)
  {
    return -1;
  }

  fs_.reserve(20);
  while ((m = getmntent(mtab)))
    fs_.push_back(initFSInfo(m));

  endmntent(mtab);
#endif

  return 0;
}


/** Figure out file system device and type ids.

    Calls stat() and statfs() on the file system to determine device
    and file system type ids.  These are required to determine if two
    paths are actually on the same file system.

    This function can be called any number of times.  It only does the
    file system check the first time the function is called. */
int
LocalFileSystem::statFSInfo(FSInfo *i)
{
  int ret = 0;
  struct stat s;
  struct statfs sfs;

  if (! i->checked)
  {
    i->checked = 1;
    if (lstat(i->dir, &s) < 0)
    {
      return -1;
    }

    if (statfs(i->dir, &sfs) < 0)
    {
      return -1;
    }

    i->dev = s.st_dev;
    i->fstype = sfs.f_type;
    if (sfs.f_bsize > 0)
    {
      i->freespc = sfs.f_bavail;
      i->freespc *= sfs.f_bsize;
      i->freespc /= 1024. * 1024. * 1024.;
    }
  }
  else if (i->fstype == -1)
  {
    errno = ENOENT;
    ret = -1;
  }

  return ret;
}


/** Find the file system @a path was mounted from.  The statfs() and
    stat() information for @a path should be in @a sfs and @a s,
    respectively.

    Finds currently mounted file system that @a path is owned by, and
    returns the FSInfo object for it, or null if no matching live file
    system can be found.  If the return value is non-null, then it is
    guaranteed @a path was on that file system.

    A null return value is possible for certain paths which are not on
    any mounted file system (e.g. /dev or /selinux), or if the file
    system is unavailable or some other way dysfunctional, such as
    dead nfs mount or filesystem does not implement statfs().  */
LocalFileSystem::FSInfo *
LocalFileSystem::findMount(const char *path, struct statfs *sfs, struct stat *s)
{
  FSInfo *best = 0;
  size_t bestlen = 0;
  size_t len = strlen(path);
  for (size_t i = 0; i < fs_.size(); ++i)
  {
    // First match simply against the file system path.  We don't
    // touch the file system until the path prefix matches.
    size_t fslen = strlen(fs_[i]->dir);
    if (! strncmp(fs_[i]->dir, path, fslen)
	&& ((fslen == 1 && fs_[i]->dir[0] == '/')
	    || len == fslen || path[fslen] == '/')
	&& (! best || fslen > bestlen))
    {
      // Get the file system device and file system ids.
      if (statFSInfo(fs_[i]) < 0)
	return 0;

      // Check the path is on the same device / file system.  If this
      // fails, we found a better prefix match on path, but it's the
      // wrong device, so reset our idea of the best match: it can't
      // be the outer mount any more.  Not sure this is the right
      // thing to do with e.g. loop-back or union mounts.
      if (fs_[i]->dev != s->st_dev || fs_[i]->fstype != sfs->f_type)
      {
	best = 0;
	continue;
      }

      // OK this is better than anything else we found so far.
      best = fs_[i];
      bestlen = fslen;
    }
  }

  return best;
}


/** Find a writeable directory among @a paths which is known to be
    local and has at least @a minFreeSpace amount of free space in
    gigabytes.

    The @a paths should contain list of relative or absolute candidate
    directories.  If an entry starts with letter "$" then the value of
    that environment variable is used instead; if the value is $TMPDIR
    and the environment variable is empty, "/tmp" is used instead.

    Returns the first path in @a paths which satisfies the criteria,
    expanded to environment variable value if appropriate, resolved
    to full absolute path.  If no suitable path can be found, returns
    an empty string.

    Does not throw exceptions.  If any serious errors occur, the errors
    are reported as message logger warnings but the actual error is
    swallowed and the directory concerned is skipped.  Non-existent
    and inaccessible directories are silently ignored without warning. */
std::string
LocalFileSystem::findCachePath(const std::vector<std::string> &paths,
			       double minFreeSpace)
{
  struct stat s;
  struct statfs sfs;

  fprintf(stderr, "Checking up to %d paths.\n", paths.size());
  for (size_t i = 0, e = paths.size(); i < e; ++i)
  {
    char *fullpath;
    const char *inpath = paths[i].c_str();
    const char *path = inpath;
    fprintf(stderr, "Checking suitability of %s for cache.\n", path);

    if (*path == '$')
    {
      char *p = getenv(path+1);
      if (p && *p)
	path = p;
      else if (! strcmp(path, "$TMPDIR"))
	path = "/tmp";
    }

    if (! (fullpath = realpath(path, 0)))
      fullpath = strdup(path);

    if (lstat(fullpath, &s) < 0)
    {
      free(fullpath);
      continue;
    }
    
    if (statfs(fullpath, &sfs) < 0)
    {
      free(fullpath);
      continue;
    }

    FSInfo *m = findMount(fullpath, &sfs, &s);

    if (m
	&& m->local
	&& m->freespc >= minFreeSpace
	&& access(fullpath, W_OK) == 0)
    {
      std::string result(fullpath);
      free(fullpath);
      return result;
    }

    free(fullpath);
  }

  return std::string();
}


/** Initialise local file system status.  */
LocalFileSystem::LocalFileSystem(void)
{
  if (readFSTypes() < 0)
    return;

  if (initFSList() < 0)
    return;
}


/** Free local file system status resources. */
LocalFileSystem::~LocalFileSystem(void)
{
  for (size_t i = 0, e = fs_.size(); i < e; ++i)
    free(fs_[i]);
}

