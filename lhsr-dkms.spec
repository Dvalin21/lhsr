%define _lhsr_name lhsr
%define _lhsr_version 1.3.0

Name:       %{_lhsr_name}-dkms
Version:    %{_lhsr_version}
Release:    1%{?dist}
Summary:    LHSR RAID kernel module (DKMS)

Group:      System Environment/Kernel
License:    GPLv3
URL:        https://github.com/Dvalin21/lhsr
Source0:    %{_lhsr_name}-%{version}.tar.gz
BuildArch:  noarch
BuildRoot:  %{_tmppath}/%{name}-%{version}-%{release}-buildroot
Requires:   dkms, make, gcc, kernel-devel

%description
Linux Hybrid Self-Healing RAID device-mapper target.
Provides RAID5/6 with CRC32c scrubbing and read-side self-healing.

This package contains the dm-lhsr kernel module source for DKMS.

%prep
%setup -q -n %{_lhsr_name}-%{version}

%build
# Source only — DKMS builds the module at install time

%install
rm -rf $RPM_BUILD_ROOT
# Install DKMS source tree
mkdir -p $RPM_BUILD_ROOT/usr/src/%{_lhsr_name}-%{version}
cp -a kernel/dm-lhsr $RPM_BUILD_ROOT/usr/src/%{_lhsr_name}-%{version}/
cp -a include $RPM_BUILD_ROOT/usr/src/%{_lhsr_name}-%{version}/
cp -a dkms.conf $RPM_BUILD_ROOT/usr/src/%{_lhsr_name}-%{version}/

%clean
rm -rf $RPM_BUILD_ROOT

%post
/sbin/dkms add -m %{_lhsr_name} -v %{version} || :
/sbin/dkms build -m %{_lhsr_name} -v %{version} || :
/sbin/dkms install -m %{_lhsr_name} -v %{version} || :

%preun
/sbin/dkms remove -m %{_lhsr_name} -v %{version} --all || :

%files
/usr/src/%{_lhsr_name}-%{version}/*

%changelog
* Tue Jun 23 2026 LHSR Team <team@lhsr.dev> - 1.3.0-1
- Initial RPM package
