CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -Werror -Isrc
BUILD   := build
LDLIBS  ?= -lm -ldl
FT_CFLAGS  := $(shell pkg-config --cflags freetype2 2>/dev/null)   # headers only: libfreetype is loaded at run time (src/ttf.c)
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)

LIB_OBJS := $(BUILD)/ne.o $(BUILD)/cpu.o $(BUILD)/fpu.o $(BUILD)/mem.o $(BUILD)/loader.o $(BUILD)/hle.o $(BUILD)/kernel.o $(BUILD)/kernel_str.o $(BUILD)/kernel_heap.o $(BUILD)/kernel_res.o $(BUILD)/kernel_file.o $(BUILD)/win87em.o $(BUILD)/dospath.o $(BUILD)/ini.o $(BUILD)/fb.o $(BUILD)/wm.o $(BUILD)/user.o $(BUILD)/gdi.o $(BUILD)/gdi_rgn.o $(BUILD)/gdi_font.o $(BUILD)/gdi_draw.o $(BUILD)/gdi_blt.o $(BUILD)/gdi_dib.o $(BUILD)/gdi_pal.o $(BUILD)/gdi_misc.o $(BUILD)/gdi_meta.o $(BUILD)/gdi_print.o $(BUILD)/gdi_ui.o $(BUILD)/toolhelp.o $(BUILD)/res.o $(BUILD)/ui.o $(BUILD)/ui_res.o $(BUILD)/ui_menu.o $(BUILD)/ui_nc.o $(BUILD)/ui_track.o $(BUILD)/ui_scroll.o $(BUILD)/ui_ctl.o $(BUILD)/ui_edit.o $(BUILD)/ui_list.o $(BUILD)/ui_dlg.o $(BUILD)/ui_misc.o $(BUILD)/inject.o $(BUILD)/install.o $(BUILD)/install_es.o $(BUILD)/kwaj.o $(BUILD)/ver.o $(BUILD)/fnt.o $(BUILD)/appicon.o $(BUILD)/ttf.o $(BUILD)/pdf.o $(BUILD)/wav.o $(BUILD)/audio.o $(BUILD)/mmsystem.o $(BUILD)/mm_wave.o $(BUILD)/mm_mci.o $(BUILD)/diag.o $(BUILD)/fsbutton.o

.PHONY: all test clean rpm
all: $(BUILD)/libne.a $(BUILD)/nedump $(BUILD)/cwfa

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c src/ne.h src/cpu.h src/fpu.h src/mem.h src/loader.h src/hle.h src/kernel.h src/kernel_priv.h src/kernel_api.h src/win87em.h src/dospath.h src/ini.h src/fb.h src/wm.h src/user.h src/gdi.h src/gdi_priv.h src/toolhelp.h src/res.h src/ui.h src/inject.h src/install.h src/install_es.h src/kwaj.h src/ver.h src/fnt.h src/appicon.h src/ttf.h src/pdf.h src/ui_priv.h src/user_priv.h src/wav.h src/audio.h src/mmsystem.h src/mm_priv.h src/diag.h src/fsbutton.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ttf.o: src/ttf.c src/ttf.h third_party/stb/stb_truetype.h | $(BUILD)
	$(CC) $(CFLAGS) $(FT_CFLAGS) -c $< -o $@

# SDL2 is confined to the presenter; everything else builds and tests without it.
$(BUILD)/present_sdl.o: src/present_sdl.c src/present_sdl.h src/fb.h src/wm.h | $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

$(BUILD)/audio_sdl.o: src/audio_sdl.c src/audio_sdl.h src/audio.h | $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -c $< -o $@

$(BUILD)/libne.a: $(LIB_OBJS)
	ar rcs $@ $^

$(BUILD)/nedump: tools/nedump.c $(BUILD)/libne.a
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ne: tests/test_ne.c $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_cpu: tests/test_cpu.c $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_fpu: tests/test_fpu.c $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_win87em: tests/test_win87em.c tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_mem: tests/test_mem.c $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_loader: tests/test_loader.c $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_modules: tests/test_modules.c tests/nebuild.h tests/dllfix.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_hle: tests/test_hle.c tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_kernel: tests/test_kernel.c tests/kfixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_kernel_heap: tests/test_kernel_heap.c tests/kfixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_kernel_modules: tests/test_kernel_modules.c tests/dllfix.h tests/nebuild.h tests/hostdir.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_kernel_res: tests/test_kernel_res.c tests/kfixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_hostfs: tests/test_hostfs.c tests/hostdir.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_kernel_file: tests/test_kernel_file.c tests/kfixture.h tests/fixture.h tests/hostdir.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_fb: tests/test_fb.c tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_wm: tests/test_wm.c tests/wmfixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_diag: tests/test_diag.c tests/wmfixture.h tests/fixture.h src/diag.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_fsbutton: tests/test_fsbutton.c tests/fixture.h src/fsbutton.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_wmq: tests/test_wmq.c tests/wmfixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_user: tests/test_user.c tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_user_msg: tests/test_user_msg.c tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_obj: tests/test_gdi_obj.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_rgn: tests/test_gdi_rgn.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_draw: tests/test_gdi_draw.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_text: tests/test_gdi_text.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_blt: tests/test_gdi_blt.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_dib: tests/test_gdi_dib.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_fon: tests/test_gdi_fon.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_ttf: tests/test_gdi_ttf.c src/ttf.h tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_print: tests/test_gdi_print.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_meta: tests/test_gdi_meta.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_pal: tests/test_gdi_pal.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_api: tests/test_gdi_api.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ui_api: tests/test_ui_api.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_res: tests/test_res.c tests/rfixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ui_res: tests/test_ui_res.c tests/rfixture.h tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ui_menu: tests/test_ui_menu.c tests/rfixture.h tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ui_nc: tests/test_ui_nc.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ui_track: tests/test_ui_track.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ui_ctl: tests/test_ui_ctl.c tests/rfixture.h tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ui_dlg: tests/test_ui_dlg.c tests/rfixture.h tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ui_edit: tests/test_ui_edit.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ui_misc: tests/test_ui_misc.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_inject: tests/test_inject.c tests/wmfixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_install: tests/test_install.c tests/hostdir.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_install_es: tests/test_install_es.c tests/hostdir.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_kwaj: tests/test_kwaj.c src/kwaj.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_pdf: tests/test_pdf.c src/pdf.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_appicon: tests/test_appicon.c src/appicon.h tests/rfixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_ttf: tests/test_ttf.c src/ttf.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_fnt: tests/test_fnt.c src/fnt.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_package: tests/test_package.c tests/fixture.h scripts/package.sh scripts/launch.sh | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_installer: tests/test_installer.c tests/fixture.h scripts/install.sh scripts/launch.sh scripts/install-desktop.sh | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_packaging: tests/test_packaging.c tests/fixture.h | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/test_ver: tests/test_ver.c tests/dllfix.h tests/nebuild.h tests/hostdir.h tests/kfixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_wav: tests/test_wav.c tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_audio: tests/test_audio.c tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_audio_sdl: tests/test_audio_sdl.c tests/fixture.h $(BUILD)/audio_sdl.o $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/audio_sdl.o $(BUILD)/libne.a -o $@ $(LDLIBS) $(SDL_LIBS)

$(BUILD)/test_mm_time: tests/test_mm_time.c tests/mmfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_mm_wave: tests/test_mm_wave.c tests/hostdir.h tests/mmfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_mm_mci: tests/test_mm_mci.c tests/mmfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_mm_api: tests/test_mm_api.c tests/mmfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_realdll: tests/test_realdll.c tests/hostdir.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_toolhelp: tests/test_toolhelp.c tests/dllfix.h tests/nebuild.h tests/hostdir.h tests/kfixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_gdi_paint: tests/test_gdi_paint.c tests/gfixture.h tests/ufixture.h tests/fixture.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/test_present: tests/test_present.c tests/wmfixture.h tests/fixture.h $(BUILD)/present_sdl.o $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/present_sdl.o $(BUILD)/libne.a -o $@ $(LDLIBS) $(SDL_LIBS)

$(BUILD)/test_cwfa: tests/test_cwfa.c tests/fixture.h $(BUILD)/cwfa $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

$(BUILD)/cwfa: tools/cwfa.c $(BUILD)/present_sdl.o $(BUILD)/audio_sdl.o $(BUILD)/libne.a
	$(CC) $(CFLAGS) $< $(BUILD)/present_sdl.o $(BUILD)/audio_sdl.o $(BUILD)/libne.a -o $@ $(LDLIBS) $(SDL_LIBS)

$(BUILD)/test_integration: tests/test_integration.c tests/fixture.h tests/hostdir.h $(BUILD)/libne.a | $(BUILD)
	$(CC) $(CFLAGS) $< $(BUILD)/libne.a -o $@ $(LDLIBS)

TESTS := test_ne test_res test_cpu test_fpu test_mem test_loader test_modules test_hle test_hostfs test_kernel test_kernel_heap test_kernel_res test_kernel_modules test_kernel_file test_win87em test_fb test_wm test_wmq test_diag test_fsbutton test_user test_user_msg test_ui_res test_ui_menu test_ui_nc test_ui_track test_ui_ctl test_ui_dlg test_ui_edit test_ui_misc test_ui_api test_inject test_install test_install_es test_kwaj test_realdll test_gdi_obj test_gdi_rgn test_gdi_draw test_gdi_text test_gdi_blt test_gdi_dib test_gdi_pal test_gdi_meta test_gdi_fon test_gdi_ttf test_gdi_print test_gdi_api test_gdi_paint test_toolhelp test_ver test_fnt test_appicon test_ttf test_pdf test_wav test_audio test_audio_sdl test_mm_time test_mm_wave test_mm_mci test_mm_api test_present test_cwfa test_integration test_package test_installer test_packaging

test: $(addprefix $(BUILD)/,$(TESTS)) $(BUILD)/nedump
	@set -e; for t in $(TESTS); do ./$(BUILD)/$$t; done

# the RPM from the committed tree (needs rpmbuild; see scripts/build-rpm.sh)
rpm:
	sh scripts/build-rpm.sh

clean:
	rm -rf $(BUILD)
