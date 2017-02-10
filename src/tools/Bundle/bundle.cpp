#include "windows.h"
#include <stdio.h>
#include "sstring.h"
#include "bundle.h"

#define PASS 0
#define FAIL -1

using namespace std;

static class Util
{
public:
    static LPCWSTR StringToUnicode(LPCSTR str)
    {
        int length = MultiByteToWideChar(CP_UTF8, 0, str, -1, NULL, 0);
        LPWSTR result = new WCHAR[length];
        MultiByteToWideChar(CP_UTF8, 0, str, -1, result, length);

        return result;
    }

    static int Error(LPCWSTR message, ...)
    {
        fwprintf(stderr, L"ERROR: ");
        va_list args;
        va_start(args, message);
        vfwprintf(stderr, message, args);
        va_end(args);
        fwprintf(stderr, L"\n");
        fflush(stderr);
        return FAIL;
    }

    static void Warn(LPCWSTR message, ...)
    {
        fwprintf(stderr, L"WARNING: ");
        va_list args;
        va_start(args, message);
        vfwprintf(stderr, message, args);
        va_end(args);
        fflush(stderr);
    }

    static void Log(LPCWSTR message, ...)
    {
        if (VerboseLog)
        {
            va_list args;
            va_start(args, message);
            vwprintf(message, args);
            va_end(args);
            fflush(stdout);
        }
    }

    static bool GetDirectory(PathString &path, PathString &name)
    {
        SString::CIterator lastBackslash = path.End();
        if (path.FindBack(lastBackslash, L'\\'))
        {
            name.Set(path, path.Begin(), lastBackslash);
            return TRUE;
        }

        return FALSE;
    }

    static void AddPath(PathString &path, PCWSTR rest)
    {
        if (!path.IsEmpty())
        {
            path.Append(L"\\");
        }
        path.Append(rest);
    }

    static bool VerboseLog;
};

bool Util::VerboseLog = false;

class InfoBuffer : public SString
{
public:
    void * GetRaw()
    {
        return (void *)GetUnicode();
    }

    COUNT_T GetByteSize()
    {
        // # strings + 1 for '\0', times Unicode width
        return (GetCount() + 1) * sizeof(WCHAR);
    }

    void AddLine(PCWSTR line)
    {
        Append(line);
        Append(L"\n");
    }
};

class Bundle
{
private:

    int BeginResourceEdits()
    {
        bundleFile = WszBeginUpdateResource(Output, FALSE);
        if (bundleFile == NULL)
        {
            return Util::Error(L"BeginUpdate failed [%d]", GetLastError());
        }
        
        return PASS;
    }

    int CommitResourceEdits()
    {
        BOOL finished = WszEndUpdateResource(bundleFile, FALSE);
        if (!finished)
        {
            return Util::Error(L"Resource Finalization failed [%d]", GetLastError());
        }

        return PASS;
    }

    int CommitResourceEditsSoFar()
    {
        if (CommitResourceEdits() == FAIL)
        {
            return FAIL;
        }
        if (BeginResourceEdits() == FAIL)
        {
            return FAIL;
        }

        return PASS;
    }

    int WriteResource(LPCWSTR resourceName, LPVOID resourceBuffer, DWORD resourceSize,
        LPCWSTR type = RT_EMBED)
    {
        // Optimize for throughput
        //
        //  Much of the time in the Bundler is spent in
        //  BeginResourceUpdate() ... EndResourceUpdate()s
        //  However the EndResourceUpdate() cannot handle arbitrary payload
        //  to write to disk, although this behavior is undocumented.
        //  Using a new transaction after every update causes a lot of overhead.
        //  Therefore, change the bundler to refresh the trancsaction
        //  after every 25 file writes.

        if (numResourcesThisTransaction > 25)
        {
            if (CommitResourceEditsSoFar() == FAIL)
            {
                return FAIL;
            }

            numResourcesThisTransaction = 0;
        }

        BOOL updated = UpdateResourceW(bundleFile, type, resourceName,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            (LPVOID)resourceBuffer, resourceSize);

        if (!updated)
        {
            return Util::Error(L"Resource Update failed %s [%d]",
                resourceName, GetLastError());
        }

        Util::Log(L" %lu bytes\n", resourceSize);
        numResourcesThisTransaction++;
        return PASS;
    }

    int BundleOne(PathString &resource, PathString &manifestName)
    {
        PCWSTR name = manifestName.GetUnicode();
        Util::Log(L"Embedding %s ... ", name);

        HANDLE resourceFile = WszCreateFile(resource, GENERIC_READ, 0, NULL,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (resourceFile == INVALID_HANDLE_VALUE)
        {
            return Util::Error(L"Invalid Handle %s [%d]", name, GetLastError());
        }

        DWORD resourceSize = GetFileSize(resourceFile, NULL);
        LPBYTE resourceBuffer = new BYTE[resourceSize];

        DWORD readSize;
        BOOL read = ReadFile(resourceFile, resourceBuffer, resourceSize, &readSize, NULL);
        if (!read)
        {
            return Util::Error(L"Read Failure %s [%d]", name, GetLastError());
        }

        CloseHandle(resourceFile);

        int written = WriteResource(manifestName, resourceBuffer, resourceSize);

        delete[] resourceBuffer;
        return written;
    }

    int BundleRecursive(PathString &resourceDir, PathString &manifestName)
    {
        WIN32_FIND_DATA iterator;
        int success = PASS;

        PathString currentDir(resourceDir);
        Util::AddPath(currentDir, L"*");

        HANDLE dirHandle = WszFindFirstFile(currentDir, &iterator);
        if (dirHandle == INVALID_HANDLE_VALUE)
        {
            return Util::Error(L"Invalid Directory %s [%d]",
                resourceDir.GetUnicode(), GetLastError());
        }

        do
        {
            PCWSTR fileName = iterator.cFileName;
            if (!wcscmp(fileName, L".") || !wcscmp(fileName, L".."))
            {
                continue;
            }

            PathString file(resourceDir);
            Util::AddPath(file, fileName);

            PathString currentManifest(manifestName);
            Util::AddPath(currentManifest, fileName);

            if (iterator.dwFileAttributes &FILE_ATTRIBUTE_DIRECTORY)
            {
                Manifest.AddLine(currentManifest);
                success |= BundleRecursive(file, currentManifest);
            }
            else
            {
                success |= BundleOne(file, currentManifest);
            }
        } while (WszFindNextFile(dirHandle, &iterator));

        FindClose(dirHandle);

        return success;
    }


public:

    Bundle()
        :bundleFile(NULL), numResourcesThisTransaction(0)
    {}

    int MakeBundle()
    {
        // Set the default bundle name
        if (Output.IsEmpty()) {
            Output.Set(L"bundle");
            Util::AddPath(Output, App);
        }

        // Create the output directory if it doesn't exist
        PathString bundleDir;
        if (Util::GetDirectory(Output, bundleDir))
        {
            WszCreateDirectory(bundleDir, NULL);
        }

        Util::Log(L"Create BundleFile %s from %s ... ",
            Output.GetUnicode(), Host.GetUnicode());
        BOOL copied = WszCopyFile(Host, Output, FALSE);
        if (!copied) {
            return Util::Error(L"Couldn't create %s [%d]",
                Output.GetUnicode(), GetLastError());
        }
        Util::Log(L"OK\n");

        if (BeginResourceEdits() == FAIL)
        {
            return FAIL;
        }

        Manifest.AddLine(App);

        PathString manifestName;
        int bundled = BundleRecursive(Resources, manifestName);

        Util::Log(L"Writing Manifest ---\n%s--- End Manifest", Manifest.GetRaw());
        int written = WriteResource(L"Manifest", Manifest.GetRaw(), Manifest.GetByteSize(), RT_EMBED_MANIFEST);

        if (CommitResourceEdits() == FAIL)
        {
            return FAIL;
        }

        return written | bundled;
    }

    bool IsValid(PCWSTR *reason)
    {
        if (Host.IsEmpty()) {
            *reason = L"Host unspecified";
            return FALSE;
        }
        if (Resources.IsEmpty()) {
            *reason = L"Resource Directory unspecified";
            return FALSE;
        }
        if (App.IsEmpty()) {
            *reason = L"App name unspecified";
            return FALSE;
        }

        PathString AppFqn(Resources);
        Util::AddPath(AppFqn, App);

        if (WszGetFileAttributes(AppFqn) == INVALID_FILE_ATTRIBUTES)
        {
            *reason = L"App not found in resource directory";
            return FALSE;
        }

        return TRUE;
    }

    PathString Host;
    PathString Output;
    PathString Resources;
    PathString App;
    InfoBuffer Manifest;

private:
    HANDLE bundleFile;
    size_t numResourcesThisTransaction;

};

static int Usage(int reason = 0)
{
    wprintf(L"CoreCLR Bundler version 0.1\n");
    wprintf(L"Usage:\n");
    wprintf(L"bundle.exe -a <App.exe>   The name of the Managed app\n");
    wprintf(L"           -h <Host.exe>  The path to CoreCLR native host\n");
    wprintf(L"           -r <resources> The directory containing all resources to embed\n");
    wprintf(L"          [-o <output>]   The path to output bundle -- defaults to .\\bundle\\App.exe\n");
    wprintf(L"          [-v]            Generate verbose output\n");
    wprintf(L"          [-?]            Display usage information\n");
    fflush(stdout);

    return reason;
}

static int UsageError(PCWSTR message)
{
    return Usage(Util::Error(message));
}

int main(int argc, char *argv[])
{
    Bundle bundle;

    char **first = argv + 1;
    char **last = argv + argc - 1;
    for (char **arg = first; arg <= last; arg++)
    {
        if (!_stricmp(*arg, "-?"))
        {
            return Usage();
        }
        else if (!_stricmp(*arg, "-v"))
        {
            Util::VerboseLog = true;
        }
        else if (!_stricmp(*arg, "-a"))
        {
            if (++arg > last) {
                return UsageError(L"-a needs an additional argument");
            }

            bundle.App.Set(Util::StringToUnicode(*arg));
        }
        else if (!_stricmp(*arg, "-h"))
        {
            if (++arg > last) {
                return UsageError(L"-h needs an additional argument");
            }
            bundle.Host.Set(Util::StringToUnicode(*arg));
        }
        else if (!_stricmp(*arg, "-r"))
        {
            if (++arg > last) {
                return UsageError(L"-r needs an additional argument");
            }
            bundle.Resources.Set(Util::StringToUnicode(*arg));
        }
        else if (!_stricmp(*arg, "-o"))
        {
            if (++arg > last) {
                return UsageError(L"-o needs an additional argument");
            }
            bundle.Output.Set(Util::StringToUnicode(*arg));
        }
        else
        {
            return Usage(Util::Error(L"Unknown command line argument %s",
                Util::StringToUnicode(*arg)));
        }
    }

    PCWSTR reason = nullptr;
    if (!bundle.IsValid(&reason)) {
        return UsageError(reason);
    }

    return bundle.MakeBundle();
}
