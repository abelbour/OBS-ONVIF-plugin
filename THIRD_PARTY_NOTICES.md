# Third-Party Notices

This project is licensed under the **GNU General Public License v3.0** (see
[`LICENSE`](LICENSE)). It builds on the following third-party works:

## obs-plugintemplate — GPL-2.0

The repository scaffold (CMake wiring, build scripts, module entry) is
vendored from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate),
Copyright (C) the OBS Project contributors, licensed under the GNU General
Public License v2.0. The template's original GPL-2.0 license text was retained
by this project's earlier revisions; the combined work is distributed under
GPL-3.0 in accordance with the project's licensing decision (`PLAN.md`).

## ONVIF Device Manager (ODM) — GPL-2.0

Protocol behavior (WS-Discovery semantics, SOAP service operations,
WS-Security digest, session shape) is ported from
[ONVIF Device Manager](https://github.com/dxball/ONVIF-Device-Manager),
licensed under the GNU General Public License v2.0. The ported logic is
incorporated into this project and distributed under GPL-3.0 (GPL-2.0 is
compatible with GPL-3.0 for the purposes of this combined work, as decided in
`PLAN.md`). ODM's `PasswordHelper.fs` (Kipod HMAC, not the ONVIF digest) is
explicitly not ported.

## OBS Studio — GPL-2.0

The plugin is an OBS Studio plugin and links against libobs /
obs-frontend-api. OBS Studio is Copyright (C) the OBS Project contributors,
licensed under the GNU General Public License v2.0. The plugin is a separate
work distributed under GPL-3.0.

## TinyXML2 — zlib license

`third_party/tinyxml2/` is vendored from
[TinyXML-2](https://github.com/leethomason/tinyxml2), Copyright (c) Lee
Thomason. Released under the zlib license:

> This software is provided 'as-is', without any express or implied
> warranty. In no event will the authors be held liable for any damages
> arising from the use of this software.
>
> Permission is granted to anyone to use this software for any purpose,
> including commercial applications, and to alter it and redistribute it
> freely, subject to the following restrictions:
>
> 1. The origin of this software must not be misrepresented; you must not
>    claim that you wrote the original software. If you use this software
>    in a product, an acknowledgment in the product documentation would be
>    appreciated but is not required.
> 2. Altered source versions must be plainly marked as such, and must not be
>    misrepresented as being the original software.
> 3. This notice may not be removed or altered from any source distribution.

## nlohmann/json — MIT License

`third_party/nlohmann/json.hpp` is vendored from
[nlohmann/json](https://github.com/nlohmann/json), Copyright (c) 2013-2023
Niels Lohmann. Released under the MIT License:

> Permission is hereby granted, free of charge, to any person obtaining a copy
> of this software and associated documentation files (the "Software"), to
> deal in the Software without restriction, including without limitation the
> rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
> sell copies of the Software, and to permit persons to whom the Software is
> furnished to do so, subject to the following conditions:
>
> The above copyright notice and this permission notice shall be included in
> all copies or substantial portions of the Software.
>
> THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
> IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
> FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
> AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
> LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
> FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
> DEALINGS IN THE SOFTWARE.

## Qt 6 — LGPL-3.0 / GPL-3.0

The plugin's user interface links against Qt 6 (Core/Widgets), provided by the
OBS Project's pre-built obs-deps/Qt6 distribution. Qt 6 is licensed under the
GNU Lesser General Public License v3.0 (with additional terms). Qt is loaded
dynamically by the host OBS Studio process; the plugin does not redistribute
Qt binaries.

## Windows SDK libraries

The plugin uses the WinHTTP, Winsock2, BCrypt and Windows Credential APIs
from the Windows SDK. These are system components provided by Microsoft; their
use is governed by the applicable Microsoft license terms and is not
distributed by this project.
