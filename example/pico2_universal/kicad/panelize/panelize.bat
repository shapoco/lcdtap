rem Rev.2

set PROJ_NAME=lcdtap_rp2350
copy %WSL_LCDTAP_DIR%\example\pico2_universal\kicad\%PROJ_NAME%.kicad_pcb

%KICAD10_SCRIPTS_PATH%\kikit.exe panelize ^
	--layout "grid; rows: 1; cols: 3; space: 2mm" ^
	--tabs "fixed; width: 5mm; hcount: 1; vcount: 2;" ^
	--cuts "mousebites; drill: 0.6mm; spacing: 1.0mm; offset: 0mm" ^
	--framing "railstb; width: 5mm; space: 2mm;" ^
	--post "millradius: 1mm" ^
	--tooling "4hole; size: 2mm; hoffset: 17mm; voffset: 2.5mm;" ^
	--fiducials "4fid; coppersize: 1mm; opening: 2mm; hoffset: 5mm; voffset: 3.85mm;" ^
	%PROJ_NAME%.kicad_pcb ^
	%PROJ_NAME%_panelized.kicad_pcb

copy %PROJ_NAME%_panelized.kicad_pcb %WSL_LCDTAP_DIR%\example\pico2_universal\panelize\%PROJ_NAME%_panelized.kicad_pcb
