# Selected RickyOS artwork in the firmware

`approved-logo.png` is the user's selected smiling cartoon portrait + dog,
circular border and **RickyOS** wordmark, copied byte-for-byte from design draft
04. It was produced with the built-in imagegen tool; its exact edit prompt and
the roles of the selected third draft / second draft layout are preserved in
`approved-logo-prompt.md`. No new generation or artistic edit was performed
for the firmware integration. Source SHA256:
`ac8c60ca236a864bb2736a2d015222aff2bc2bb667f3d7248c3d9f234863a12f`.

Developer-only generation uses Node.js + sharp 0.35.4. Set `NODE_PATH` if sharp
is installed in a separate runtime:

```sh
node scripts/build_rickyos_logo.cjs
node scripts/build_rickyos_logo.cjs --out /tmp/logo.h --preview /tmp/logo.png
```

The generator verifies the original source hash, removes only surrounding
white padding, fits proportionally into 384×448 and packs a fixed 1-bpp mask.
The real enclosing circle is isolated from the source as a connected component,
not redrawn. Its mask is restricted to the complete logo's black pixels.
Both masks total 43,008 bytes of immutable Flash. Checked-in `images/RickyLogo.h`
is generated output; edit the generator/source, never its byte arrays.

Boot displays the complete artwork in one full refresh, without animation,
artificial dwell or a version footer. Normal wake/PostOTA do not replay it.
Power-off transition/default standby use
the complete artwork. The old native R mark and duplicate text wordmark are
removed. Other standby choices stay unchanged.

Rendering goes through the product theme and original framebuffer. It uses
direct constant-byte reads and bounded coordinate mapping, with no heap
allocation, PNG decoder, SD dependency or additional framebuffer. Asset size
caps enlargement; the safe area constrains portrait/landscape placement.
Physical e-paper ghosting and legibility remain device acceptance tasks.
