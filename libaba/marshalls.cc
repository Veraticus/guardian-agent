#include "marshalls.hh"

#include <cassert>
#include <fcntl.h>
#include <syscall.h>
#include <experimental/filesystem>

namespace guardian_agent {

using google::protobuf::RepeatedPtrField;    
namespace fs = std::experimental::filesystem;

SyscallMarshall::Registry SyscallMarshall::registry;

template<class T>
class Registrar
{
public:
    Registrar(int syscall_number) 
    { 
        assert(SyscallMarshall::registry.find(syscall_number) == SyscallMarshall::registry.end());
        SyscallMarshall::registry[syscall_number] = []() { return new T; }; 
    }
};

#define REGISTER_SYSCALL_MARSHAL(sycall_number, class_name) \
static Registrar<class_name> register_##sycall_number(sycall_number);

class FdProcessor : public ResultProcessor 
{
public:
    FdProcessor(long int* result)
    : result(result)
    {}

    bool Process(const Argument& arg) 
    {
        if (!arg.has_fd_arg()) {
            return false;
        }
        *result = arg.fd_arg().fd();
        return true;
    }

private:
    long int* result;
};

SyscallMarshall* SyscallMarshall::New(long syscall_number, long arg0, long arg1, long arg2, long arg3, long arg4, long arg5, long int* result)
{
    auto factory_func = registry.find((int)syscall_number);
    if (factory_func == registry.end()) {
        return nullptr;
    } else {
        auto marshall = (factory_func->second)();
        marshall->arg0 = arg0;
        marshall->arg1 = arg1;
        marshall->arg2 = arg2;
        marshall->arg3 = arg3;
        marshall->arg4 = arg4;
        marshall->arg5 = arg5;
        marshall->result = result;
        marshall->Prepare();
        return marshall;
    }
}

void SyscallMarshall::ProcessResponse(const ElevationResponse& response) 
{ 
    *result = -response.errno_code();
    if (response.results_size() > (int)result_processors.size()) {
        *result = -1;
        return;
    }
    for (int i = 0; i < response.results_size(); ++i) {
        if (!result_processors[i]->Process(response.results(i))) {
            *result = -1;
        }
    }
}

template<typename T>
class FromAtSyscall : public T
{
public:
    void Prepare()
    {
        T::arg5 = T::arg4;
        T::arg4 = T::arg3;
        T::arg3 = T::arg2;
        T::arg2 = T::arg1;
        T::arg1 = T::arg0;
        T::arg0 = AT_FDCWD;
        T::Prepare();
    }
};

class OpenAtMarshall : public SyscallMarshall 
{
public:
    void Prepare() 
    {
        args.Add()->mutable_dir_fd_arg()->set_fd(arg0);
        args.Add()->set_string_arg((char*)arg1);
        args.Add()->set_int_arg(arg2);
        args.Add()->set_int_arg(arg3);
        result_processors.push_back(std::unique_ptr<ResultProcessor>(new FdProcessor(result)));
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_openat, OpenAtMarshall)
REGISTER_SYSCALL_MARSHAL(SYS_open, FromAtSyscall<OpenAtMarshall>)

class MkdirAtMarshall : public SyscallMarshall 
{
public:
    void Prepare() 
    {
        args.Add()->mutable_dir_fd_arg()->set_fd(arg0);
        args.Add()->set_string_arg((char*)arg1);
        args.Add()->set_int_arg(arg2);
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_mkdirat, MkdirAtMarshall)
REGISTER_SYSCALL_MARSHAL(SYS_mkdir, FromAtSyscall<MkdirAtMarshall>)

class SymlinkAtMarshall : public SyscallMarshall 
{
public:
    void Prepare() 
    {
        args.Add()->set_string_arg((char*)arg0);
        args.Add()->mutable_dir_fd_arg()->set_fd(arg1);
        args.Add()->set_string_arg((char*)arg2);
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_symlinkat, SymlinkAtMarshall)

class SymlinkMarshall : public SymlinkAtMarshall 
{
public:
    void Prepare() 
    {
        arg2 = arg1;
        arg1 = AT_FDCWD;
        SymlinkAtMarshall::Prepare();
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_symlink, SymlinkMarshall)

class UnlinkAtMarshall : public SyscallMarshall 
{
public:
    void Prepare() 
    {
        args.Add()->mutable_dir_fd_arg()->set_fd(arg0);
        args.Add()->set_string_arg((char*)arg1);
        args.Add()->set_int_arg(arg2);
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_unlinkat, UnlinkAtMarshall)

class UnlinkMarshall : public UnlinkAtMarshall 
{
public:
    void Prepare() 
    {
        arg2 = 0;
        arg1 = arg0;
        arg0 = AT_FDCWD;
        UnlinkAtMarshall::Prepare();
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_unlink, UnlinkMarshall)

class RmdirMarshall : public UnlinkAtMarshall 
{
public:
    void Prepare() 
    {
        arg2 = AT_REMOVEDIR;
        arg1 = arg0;
        arg0 = AT_FDCWD;
        UnlinkAtMarshall::Prepare();
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_rmdir, RmdirMarshall)

class FaccessAtMarshall : public SyscallMarshall 
{
public:
    void Prepare() 
    {
        args.Add()->mutable_dir_fd_arg()->set_fd(arg0);
        args.Add()->set_string_arg((char*)arg1);
        args.Add()->set_int_arg(arg2);
        args.Add()->set_int_arg(arg3);
    }

    bool ShouldHook() 
    {
        // Don't try to elevate executable access checks for files that
        // are not executable at all. 
        mode_t mode = (mode_t)arg2;
        if (mode == X_OK) {
            auto p = fs::status((char*)arg1).permissions();
            if (((p & fs::perms::owner_exec) == fs::perms::none) &&
                ((p & fs::perms::group_exec) == fs::perms::none) &&
                ((p & fs::perms::others_exec) == fs::perms::none)) {
                return false;
            }
        }
        return true;
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_faccessat, FaccessAtMarshall)

class AccessMarshall : public FaccessAtMarshall 
{
public:
    void Prepare() 
    {
        arg3 = 0;
        arg2 = arg1;
        arg1 = arg0;
        arg0 = AT_FDCWD;
        FaccessAtMarshall::Prepare();
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_access, AccessMarshall)

class SocketMarshall : public SyscallMarshall 
{
public:
    void Prepare() 
    {
        args.Add()->set_int_arg(arg0);
        args.Add()->set_int_arg(arg1);
        args.Add()->set_int_arg(arg2);
        result_processors.push_back(std::unique_ptr<ResultProcessor>(new FdProcessor(result)));
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_socket, SocketMarshall)

class BindMarshall : public SyscallMarshall 
{
public:
    void Prepare() 
    {
        args.Add()->mutable_fd_arg()->set_fd(arg0);
        args.Add()->set_bytes_arg(std::string((const char*)arg1, arg2));
    }
};
REGISTER_SYSCALL_MARSHAL(SYS_bind, BindMarshall)

}  // namespace guardian_agent