Name:           hp-supplies
Version:        1.2.1
Release:        1%{?dist}
Summary:        HP 打印机耗材与用量监控（LEDM over USB）

License:        MIT
URL:            https://localhost/hp-supplies
Source0:        %{name}-%{version}.tar.gz
BuildRequires:  gcc, make, pkgconfig(gtk+-3.0), pkgconfig(libusb-1.0)
Requires:       gtk3, libusb1

%description
通过 USB LEDM 接口查询 HP 打印机的碳粉/墨水余量、
耗材状态与总打印页数。协议逆向自 HP Windows 打印机助手。适用于带 "HP LEDM" USB 接口的
HP 打印机（较新的 InkJet / LaserJet / Smart Tank 等系列）。
无需 HPLIP。提供 GTK3 图形界面与 --text 命令行模式。

%prep
%setup -q

%build
%make_build APP_VERSION=%{version}-%{release}

%install
%make_install APP_VERSION=%{version}-%{release}

%post
udevadm control --reload >/dev/null 2>&1 || true
udevadm trigger >/dev/null 2>&1 || true

%files
%license LICENSE
%doc README.md
%{_bindir}/hp-supplies
%{_datadir}/applications/hp-supplies.desktop
%{_datadir}/icons/hicolor/scalable/apps/hp-supplies.svg
/usr/lib/udev/rules.d/60-hp-supplies.rules

%changelog
* Sat Oct 24 2026 londy <londy@local> - 1.0.0-1
- Initial package
