//===--- Solaris.cpp - Solaris ToolChain Implementations --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Solaris.h"
#include "CommonArgs.h"
#include "clang/Config/config.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Options.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

using namespace clang::driver;
using namespace clang::driver::tools;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

static const char * getUPCLibDir(const llvm::Triple &Triple) {
  if (Triple.getArch() == llvm::Triple::x86_64 &&
      Triple.getEnvironment() == llvm::Triple::GNUX32)
    return "/x32";

  return Triple.isArch32Bit() ? "/32" : "";
}


static const char *GetUPCLibOption(const ArgList &Args) {
  llvm::SmallString<32> Buf("-lupc");
  if (Args.getLastArgValue(options::OPT_fupc_pts_EQ, "packed") == "struct") {
    Buf += "-s";
  }
  if (Args.getLastArgValue(options::OPT_fupc_pts_vaddr_order_EQ, "first") == "last") {
    Buf += "-l";
  }
  if (Arg * A = Args.getLastArg(options::OPT_fupc_packed_bits_EQ)) {
    llvm::SmallVector<llvm::StringRef, 3> Bits;
    StringRef(A->getValue()).split(Bits, ",");
    bool okay = true;
    int Values[3];
    if (Bits.size() == 3) {
      for (int i = 0; i < 3; ++i)
        if (Bits[i].getAsInteger(10, Values[i]) || Values[i] <= 0)
          okay = false;
      if (Values[0] + Values[1] + Values[2] != 64)
        okay = false;
    } else {
      okay = false;
    }
    if (okay) {
      if(Values[0] != 20 || Values[1] != 10 || Values[2] != 34) {
        Buf += "-";
        Buf += Bits[0];
        Buf += "-";
        Buf += Bits[1];
        Buf += "-";
        Buf += Bits[2];
      }
    }
  }
  return Args.MakeArgString(Buf);
}

static const char *GetUPCBeginFile(const ArgList &Args) {
  const char *upc_crtbegin;
  if (Args.hasArg(options::OPT_static))
    upc_crtbegin = "upc-crtbeginT.o";
  else if (Args.hasArg(options::OPT_shared) || Args.hasArg(options::OPT_pie))
    upc_crtbegin = "upc-crtbeginS.o";
  else
    upc_crtbegin = "upc-crtbegin.o";
  return upc_crtbegin;
}

static const char *GetUPCEndFile(const ArgList &Args) {
  const char *upc_crtend;
  if (Args.hasArg(options::OPT_static))
    upc_crtend = "upc-crtendT.o";
  else if (Args.hasArg(options::OPT_shared) || Args.hasArg(options::OPT_pie))
    upc_crtend = "upc-crtendS.o";
  else
    upc_crtend = "upc-crtend.o";
  return upc_crtend;
}

void solaris::Assembler::ConstructJob(Compilation &C, const JobAction &JA,
                                      const InputInfo &Output,
                                      const InputInfoList &Inputs,
                                      const ArgList &Args,
                                      const char *LinkingOutput) const {
  claimNoWarnArgs(Args);
  ArgStringList CmdArgs;

  Args.AddAllArgValues(CmdArgs, options::OPT_Wa_COMMA, options::OPT_Xassembler);

  CmdArgs.push_back("-o");
  CmdArgs.push_back(Output.getFilename());

  for (const auto &II : Inputs)
    CmdArgs.push_back(II.getFilename());

  const char *Exec = Args.MakeArgString(getToolChain().GetProgramPath("as"));
  C.addCommand(llvm::make_unique<Command>(JA, *this, Exec, CmdArgs, Inputs));
}

void solaris::Linker::ConstructJob(Compilation &C, const JobAction &JA,
                                   const InputInfo &Output,
                                   const InputInfoList &Inputs,
                                   const ArgList &Args,
                                   const char *LinkingOutput) const {
  ArgStringList CmdArgs;

  // Demangle C++ names in errors
  CmdArgs.push_back("-C");

  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_shared)) {
    CmdArgs.push_back("-e");
    CmdArgs.push_back("_start");
  }

  if (Args.hasArg(options::OPT_static)) {
    CmdArgs.push_back("-Bstatic");
    CmdArgs.push_back("-dn");
  } else {
    CmdArgs.push_back("-Bdynamic");
    if (Args.hasArg(options::OPT_shared)) {
      CmdArgs.push_back("-shared");
    }

    // libpthread has been folded into libc since Solaris 10, no need to do
    // anything for pthreads. Claim argument to avoid warning.
    Args.ClaimAllArgs(options::OPT_pthread);
    Args.ClaimAllArgs(options::OPT_pthreads);
  }

  if (Output.isFilename()) {
    CmdArgs.push_back("-o");
    CmdArgs.push_back(Output.getFilename());
  } else {
    assert(Output.isNothing() && "Invalid output.");
  }

  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)) {
    if (!Args.hasArg(options::OPT_shared))
      CmdArgs.push_back(
          Args.MakeArgString(getToolChain().GetFilePath("crt1.o")));

    CmdArgs.push_back(Args.MakeArgString(getToolChain().GetFilePath("crti.o")));
    CmdArgs.push_back(
        Args.MakeArgString(getToolChain().GetFilePath("values-Xa.o")));
    CmdArgs.push_back(
        Args.MakeArgString(getToolChain().GetFilePath("crtbegin.o")));
    if (getToolChain().getDriver().CCCIsUPC()) {
      const char *upc_crtbegin = GetUPCBeginFile(Args);
      CmdArgs.push_back(Args.MakeArgString(getToolChain().GetFilePath(upc_crtbegin)));
    }
  }

  getToolChain().AddFilePathLibArgs(Args, CmdArgs);

  Args.AddAllArgs(CmdArgs, {options::OPT_L, options::OPT_T_Group,
                            options::OPT_e, options::OPT_r});

  bool NeedsSanitizerDeps = addSanitizerRuntimes(getToolChain(), Args, CmdArgs);
  AddLinkerInputs(getToolChain(), Inputs, Args, CmdArgs, JA);

  if (getToolChain().getDriver().CCCIsUPC() && !Args.hasArg(options::OPT_nostdlib)) {
    CmdArgs.push_back(GetUPCLibOption(Args));
#ifdef LIBUPC_ENABLE_OMP_CHECKS
    CmdArgs.push_back("-lpthread");
#endif
  }


  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nodefaultlibs)) {
    if (getToolChain().getDriver().CCCIsUPC()) {
      const char *upc_crtend = GetUPCEndFile(Args);
      CmdArgs.push_back(Args.MakeArgString(getToolChain().GetFilePath(upc_crtend)));
    }
    if (getToolChain().ShouldLinkCXXStdlib(Args))
      getToolChain().AddCXXStdlibLibArgs(Args, CmdArgs);
    if (Args.hasArg(options::OPT_fstack_protector) ||
        Args.hasArg(options::OPT_fstack_protector_strong) ||
        Args.hasArg(options::OPT_fstack_protector_all)) {
      // Explicitly link ssp libraries, not folded into Solaris libc.
      CmdArgs.push_back("-lssp_nonshared");
      CmdArgs.push_back("-lssp");
    }
    CmdArgs.push_back("-lgcc_s");
    CmdArgs.push_back("-lc");
    if (!Args.hasArg(options::OPT_shared)) {
      CmdArgs.push_back("-lgcc");
      CmdArgs.push_back("-lm");
    }
    if (NeedsSanitizerDeps)
      linkSanitizerRuntimeDeps(getToolChain(), CmdArgs);
  }

  if (!Args.hasArg(options::OPT_nostdlib, options::OPT_nostartfiles)) {
    CmdArgs.push_back(
        Args.MakeArgString(getToolChain().GetFilePath("crtend.o")));
  }
  CmdArgs.push_back(Args.MakeArgString(getToolChain().GetFilePath("crtn.o")));

  getToolChain().addProfileRTLibs(Args, CmdArgs);

  const char *Exec = Args.MakeArgString(getToolChain().GetLinkerPath());
  C.addCommand(llvm::make_unique<Command>(JA, *this, Exec, CmdArgs, Inputs));
}

static StringRef getSolarisLibSuffix(const llvm::Triple &Triple) {
  switch (Triple.getArch()) {
  case llvm::Triple::x86:
  case llvm::Triple::sparc:
    break;
  case llvm::Triple::x86_64:
    return "/amd64";
  case llvm::Triple::sparcv9:
    return "/sparcv9";
  default:
    llvm_unreachable("Unsupported architecture");
  }
  return "";
}

/// Solaris - Solaris tool chain which can call as(1) and ld(1) directly.

Solaris::Solaris(const Driver &D, const llvm::Triple &Triple,
                 const ArgList &Args)
    : Generic_ELF(D, Triple, Args) {

  GCCInstallation.init(Triple, Args);

  StringRef LibSuffix = getSolarisLibSuffix(Triple);
  path_list &Paths = getFilePaths();
  if (GCCInstallation.isValid()) {
    // On Solaris gcc uses both an architecture-specific path with triple in it
    // as well as a more generic lib path (+arch suffix).
    addPathIfExists(D,
                    GCCInstallation.getInstallPath() +
                        GCCInstallation.getMultilib().gccSuffix(),
                    Paths);
    addPathIfExists(D, GCCInstallation.getParentLibPath() + LibSuffix, Paths);
  }

  // If we are currently running Clang inside of the requested system root,
  // add its parent library path to those searched.
  if (StringRef(D.Dir).startswith(D.SysRoot))
    addPathIfExists(D, D.Dir + "/../lib", Paths);

  addPathIfExists(D, getDriver().SysRoot + getDriver().Dir + "/../lib" + getUPCLibDir(Triple), Paths);
  addPathIfExists(D, D.SysRoot + "/usr/lib" + LibSuffix, Paths);
}

SanitizerMask Solaris::getSupportedSanitizers() const {
  const bool IsX86 = getTriple().getArch() == llvm::Triple::x86;
  SanitizerMask Res = ToolChain::getSupportedSanitizers();
  // FIXME: Omit X86_64 until 64-bit support is figured out.
  if (IsX86) {
    Res |= SanitizerKind::Address;
    Res |= SanitizerKind::PointerCompare;
    Res |= SanitizerKind::PointerSubtract;
  }
  Res |= SanitizerKind::Vptr;
  return Res;
}

Tool *Solaris::buildAssembler() const {
  return new tools::solaris::Assembler(*this);
}

Tool *Solaris::buildLinker() const { return new tools::solaris::Linker(*this); }

void Solaris::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                        ArgStringList &CC1Args) const {
  const Driver &D = getDriver();

  if (DriverArgs.hasArg(clang::driver::options::OPT_nostdinc))
    return;

  if (!DriverArgs.hasArg(options::OPT_nostdlibinc))
    addSystemInclude(DriverArgs, CC1Args, D.SysRoot + "/usr/local/include");

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc)) {
    SmallString<128> P(D.ResourceDir);
    llvm::sys::path::append(P, "include");
    addSystemInclude(DriverArgs, CC1Args, P);
  }

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // Check for configure-time C include directories.
  StringRef CIncludeDirs(C_INCLUDE_DIRS);
  if (CIncludeDirs != "") {
    SmallVector<StringRef, 5> dirs;
    CIncludeDirs.split(dirs, ":");
    for (StringRef dir : dirs) {
      StringRef Prefix =
          llvm::sys::path::is_absolute(dir) ? StringRef(D.SysRoot) : "";
      addExternCSystemInclude(DriverArgs, CC1Args, Prefix + dir);
    }
    return;
  }

  // Add include directories specific to the selected multilib set and multilib.
  if (GCCInstallation.isValid()) {
    const MultilibSet::IncludeDirsFunc &Callback =
        Multilibs.includeDirsCallback();
    if (Callback) {
      for (const auto &Path : Callback(GCCInstallation.getMultilib()))
        addExternCSystemIncludeIfExists(
            DriverArgs, CC1Args, GCCInstallation.getInstallPath() + Path);
    }
  }

  addExternCSystemInclude(DriverArgs, CC1Args, D.SysRoot + "/usr/include");
}

void Solaris::addLibStdCxxIncludePaths(
    const llvm::opt::ArgList &DriverArgs,
    llvm::opt::ArgStringList &CC1Args) const {
  // We need a detected GCC installation on Solaris (similar to Linux)
  // to provide libstdc++'s headers.
  if (!GCCInstallation.isValid())
    return;

  // By default, look for the C++ headers in an include directory adjacent to
  // the lib directory of the GCC installation.
  // On Solaris this usually looks like /usr/gcc/X.Y/include/c++/X.Y.Z
  StringRef LibDir = GCCInstallation.getParentLibPath();
  StringRef TripleStr = GCCInstallation.getTriple().str();
  const Multilib &Multilib = GCCInstallation.getMultilib();
  const GCCVersion &Version = GCCInstallation.getVersion();

  // The primary search for libstdc++ supports multiarch variants.
  addLibStdCXXIncludePaths(LibDir.str() + "/../include", "/c++/" + Version.Text,
                           TripleStr,
                           /*GCCMultiarchTriple*/ "",
                           /*TargetMultiarchTriple*/ "",
                           Multilib.includeSuffix(), DriverArgs, CC1Args);
}
