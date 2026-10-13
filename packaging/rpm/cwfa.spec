Name:           cwfa
Version:        0.1.0
Release:        2%{?dist}
Summary:        Native Linux runtime for Creative Writer and Fine Artist (Windows 3.1)

# the stb_truetype header carries a dual MIT / public-domain licence
License:        MIT AND (MIT OR Unlicense)
URL:            https://github.com/MAlexVR/creative-writer-fine-artist-linux
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  pkgconfig(freetype2)
BuildRequires:  desktop-file-utils
BuildRequires:  appstream

# the TrueType faces that stand in for Arial, Times New Roman and Courier New (none are bundled)
Requires:       liberation-sans-fonts
Requires:       liberation-serif-fonts
Requires:       liberation-mono-fonts
# unpacking a CD disc image and asking where the CD is
Recommends:     (7zip or bsdtar or genisoimage)
Recommends:     zenity
# hinted 1-bit TrueType text (loaded at run time when present)
Recommends:     freetype

%description
cwfa runs the original, unmodified Windows 3.1 programs Microsoft Creative Writer and Fine Artist
(English and Spanish editions) on Linux, without Wine: an x86 16-bit interpreter and a native
implementation of the Win16 API drawing to an SDL2 window.

This package contains no Microsoft software. You need your own copy of the CD or its disc image;
the launcher asks for it on the first start and remembers it (~/.config/cwfa/media.conf). Users and
documents are kept in ~/.local/share/cwfa/data; printing saves PDF files in your Documents folder.

It installs the runtime and the cwfa-launch command only. The application-menu entries of each edition
come in their own packages: cwfa-english and cwfa-spanish (install either one, or both).

%package english
Summary:        Application-menu entries for the English editions of Creative Writer and Fine Artist
Requires:       cwfa = %{version}-%{release}

%description english
Launchers and application-store metadata for the English editions of Microsoft Creative Writer and
Fine Artist. Needs your own CD or disc image of those editions.

%package spanish
Summary:        Application-menu entries for the Spanish editions of Creative Writer and Fine Artist
Requires:       cwfa = %{version}-%{release}

%description spanish
Launchers and application-store metadata for the Spanish editions of Microsoft Creative Writer and
Fine Artist, listed as "Creative Writer" and "Fine Artist". Needs your own copy of the Spanish disks.

%prep
%autosetup

%build
# the Makefile appends its own warning and include options to the CFLAGS of the environment
%set_build_flags
LDLIBS="$LDFLAGS -lm -ldl" make %{?_smp_mflags} build/cwfa

%install
install -Dpm 0755 build/cwfa %{buildroot}%{_libexecdir}/cwfa/bin/cwfa
install -Dpm 0755 scripts/launch.sh %{buildroot}%{_libexecdir}/cwfa/cwfa-launch
install -Dpm 0755 scripts/install-desktop.sh %{buildroot}%{_libexecdir}/cwfa/cwfa-install-desktop
install -d %{buildroot}%{_bindir}
ln -s ../libexec/cwfa/cwfa-launch %{buildroot}%{_bindir}/cwfa-launch
desktop-file-install --dir=%{buildroot}%{_datadir}/applications packaging/desktop/cwfa-writer.desktop packaging/desktop/cwfa-artist.desktop \
    packaging/desktop/cwfa-es-writer.desktop packaging/desktop/cwfa-es-artist.desktop
for id in writer artist es-writer es-artist; do
    install -Dpm 0644 packaging/icons/cwfa-$id.svg %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/cwfa-$id.svg
done
for id in writer artist writer_es artist_es; do
    install -Dpm 0644 packaging/appstream/io.github.malexvr.cwfa.$id.metainfo.xml %{buildroot}%{_metainfodir}/io.github.malexvr.cwfa.$id.metainfo.xml
done

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/cwfa-*.desktop
appstreamcli validate --no-net %{buildroot}%{_metainfodir}/*.metainfo.xml

%files
%license LICENSE
%doc README.md README.es.md
%{_bindir}/cwfa-launch
%{_libexecdir}/cwfa/

%files english
%{_datadir}/applications/cwfa-writer.desktop
%{_datadir}/applications/cwfa-artist.desktop
%{_datadir}/icons/hicolor/scalable/apps/cwfa-writer.svg
%{_datadir}/icons/hicolor/scalable/apps/cwfa-artist.svg
%{_metainfodir}/io.github.malexvr.cwfa.writer.metainfo.xml
%{_metainfodir}/io.github.malexvr.cwfa.artist.metainfo.xml

%files spanish
%{_datadir}/applications/cwfa-es-writer.desktop
%{_datadir}/applications/cwfa-es-artist.desktop
%{_datadir}/icons/hicolor/scalable/apps/cwfa-es-writer.svg
%{_datadir}/icons/hicolor/scalable/apps/cwfa-es-artist.svg
%{_metainfodir}/io.github.malexvr.cwfa.writer_es.metainfo.xml
%{_metainfodir}/io.github.malexvr.cwfa.artist_es.metainfo.xml

%changelog
* Mon Oct 12 2026 MAlexVR <6804880+MAlexVR@users.noreply.github.com> - 0.1.0-2
- Split the launchers by edition: cwfa (runtime only), cwfa-english and cwfa-spanish (menu entries, AppStream)
* Mon Oct 12 2026 MAlexVR <6804880+MAlexVR@users.noreply.github.com> - 0.1.0-1
- First package: native runtime, launcher, desktop entries and AppStream metadata
