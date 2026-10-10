** @name playcdi.s Play CD-i application module.
*
* This module implements a dummy player shell that will cause immediate
* playing of the CD-i application by exiting to the launcher process
* with the right exit status. When linked it should be given the
* module name "play" so that it will override the player shell module
* in the ROMs, just like the "cdistub" program from the CD-i Stub
* distribution.
*
* @section Copyright (c) 2005, CD-i Fan.
* This file is licensed under the GNU Library General Public License,
* version 2 or (at your option) any later version. The full terms of
* the license can be found in the file LCOPYING that you should have
* received with this file. You can also refer to the on-line version that
* can be found at http://www.fsf.org/licensing/licenses/lgpl.txt.
*
* @author CD-i Fan
*
* @version 0.5.1
*
* @history
* @rev 2005/08/18 cdifan Created
* @rev 2005/09/04 cdifan Released

		use <oskdefs.d>
		
* Use maximum rev# to override player shell in ROM.
TypeLang	equ	(Prgrm<<8)!Objct
AttrRev	equ	(ReEnt<<8)!255

* First edition of this module.
Edit		equ	1

* Use minimum stack.
Stack		equ	0

		psect		playcdi,TypeLang,AttrRev,Edit,Stack,start
		
** Exit codes for player shell modules, from the Philips document
* "Technical Documentation For CDI 605 Users" that is supposed to
* come with a CD-i 605 player.

START_CDI		equ	1
START_FLOPPY	equ	2
START_SHELL		equ	3

** Entry point of play CD-i module.

start:
		moveq		#START_CDI,d1
		os9		F$Exit

		ends
