include(ECMInstallIcons)

# line items and other pictograms
set(PICTOGRAMS
	brkena.png
	brkdis.png
	brktmp.png
	brkcond.png
	brkorph.png
	pcinner.png
	pcup.png
	watchena.png
	watchdis.png
	pointer.xpm)

install(FILES ${PICTOGRAMS} DESTINATION "${DATA_INSTALL_DIR}/kdbg/pics")

ecm_install_icons("${DATA_INSTALL_DIR}/kdbg/icons")
