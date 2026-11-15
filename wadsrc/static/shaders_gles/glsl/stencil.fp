/*
** stencil.fp
**
**
**
**---------------------------------------------------------------------------
**
** Copyright 2014-2016 Christoph Oelckers
** Copyright 2017-2025 GZDoom Maintainers and Contributors
** Copyright 2025-2026 UZDoom Maintainers and Contributors
**
** SPDX-License-Identifier: GPL-3.0-or-later
**
**---------------------------------------------------------------------------
**
*/

void main()
{
	// Normally drawn with color writes off. Skipped portals use it to fill their area with the object color.
	gl_FragColor = vec4(uObjectColor.rgb, 1.0);
}
