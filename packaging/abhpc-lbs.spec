Name:           abhpc-lbs
Version:        1.0.0
Release:        2%{?dist}
Summary:        Dynamic node weight balancing for Slurm clusters
License:        MIT
URL:            https://github.com/abhpc/abhpc-lbs
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  python3 >= 3.6

%description
Compute configurable Slurm node weights from controller-reported CPU load
with grouped updates. Includes an optional systemd timer, disabled by default.
Requires an existing Slurm installation; set scontrol_path for custom locations.

%prep
%setup -q

%build
make %{?_smp_mflags} CFLAGS="%{optflags}" LDFLAGS="%{?build_ldflags}"

%install
make install DESTDIR=%{buildroot} PREFIX=%{_prefix} BINDIR=%{_bindir} SYSCONFDIR=%{_sysconfdir}

%check
make test

%files
%license LICENSE
%doc README.md README.en.md
%{_bindir}/abhpc-lbs
%config(noreplace) %{_sysconfdir}/abhpc-lbs.conf
/usr/lib/systemd/system/abhpc-lbs.service
/usr/lib/systemd/system/abhpc-lbs.timer

%changelog
* Sat Nov 07 2026 abhpc-lbs maintainers - 1.0.0-2
- Skip unavailable nodes without failing an otherwise successful run.

* Sat Nov 07 2026 abhpc-lbs maintainers - 1.0.0-1
- C implementation using Slurm CPULoad, configurable formulas and batched updates.
