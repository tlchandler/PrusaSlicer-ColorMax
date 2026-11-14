# PrusaSlicer-ColorMax

**PrusaSlicer-ColorMax prints full-color models on a multi-tool printer using just a handful of
filaments.** Thin layers of different colors, printed one after another, blend into a new color when
seen from a normal distance: a few layers of Cyan and Yellow look green, Magenta and White look pink.
ColorMax takes a colored model (a painted or multi-color 3MF from PrusaSlicer, Bambu Studio, OrcaSlicer, ...), works out a layer recipe for every
color in it, shows you how each color will print, and repaints the model so PrusaSlicer prints those
recipes. With the five Prusament PLA ColorMix colors and two walls of different recipes it can print
roughly 2,000 visibly different colors.

To get the colors right, ColorMax learns how your spools actually mix on your printer: you print a
calibration plate of color swatches, scan it on a flatbed scanner, and ColorMax measures them. The
Prusament PLA ColorMix set ships with a reference calibration; with your own, predicted colors are
typically within about 2 ΔE of the print (hard to tell apart).

More: [what ColorMax does and how](doc/ColorMax.md) - [changelog](COLORMAX_CHANGELOG.md).

Three things to know:

1. **It is vibe-coded.** It is written with an AI coding assistant, directed and reviewed by someone
   who understands code, with automated tests for what can be tested. But it is vibe-coded: treat it
   as experimental, and check the preview before you print something that matters.

2. **It builds on the ideas of [iDither](https://idither.com), not on its code.** Mixing colors by
   alternating thin layers was pioneered by [Full Spectrum](https://github.com/ratdoux/OrcaSlicer-FullSpectrum)
   (an open-source OrcaSlicer fork). Prusa then brought layer color mixing into PrusaSlicer through
   [virtual extruders and ColorMix](https://blog.prusa3d.com/our-new-open-source-colormix-model-in-prusaslicer-and-easyprint_136079/).
   iDither went its own way with these ideas (calibrated recipes, different inner and outer walls), and
   ColorMax carries them forward. We don't use iDither's code in any way. We are uncomfortable with iDither for a variety of reasons, including:
   - It isn't open source. Its license is proprietary and its source is private, so nobody can check
     how it works, fix it or build on it.  But it is built on the back of many other open source projects.
   - It puts a website in the middle of your workflow. You take your model out of the slicer to
     idither.com (the analysis runs in your browser; downloading the result needs an account made with
     a referral code), then bring a project file back into Bambu Studio or Snapmaker Orca. The site can
     change its terms or disappear at any time.
   - Some elements of it cost (the app).

   ColorMax runs entirely inside PrusaSlicer on your computer, and all of its code is here, under the
   same open-source license as PrusaSlicer.

3. **It is a fork of PrusaSlicer 2.9.7**, the current stable release. GitHub shows it as thousands of
   commits "behind" Prusa's `master` branch: that branch is PrusaSlicer 3.0, still in alpha. ColorMax
   will be ported to PrusaSlicer 3.0 once 3.0 is stable.

---

*Everything below this line is Prusa's original PrusaSlicer README.*

---


![PrusaSlicer logo](/resources/icons/PrusaSlicer.png?raw=true)

# PrusaSlicer

You may want to check the [PrusaSlicer project page](https://www.prusa3d.com/prusaslicer/).
Prebuilt Windows, OSX and Linux binaries are available through the [git releases page](https://github.com/prusa3d/PrusaSlicer/releases) or from the [Prusa3D downloads page](https://www.prusa3d.com/drivers/). There are also [3rd party Linux builds available](https://github.com/prusa3d/PrusaSlicer/wiki/PrusaSlicer-on-Linux---binary-distributions).

PrusaSlicer takes 3D models (STL, OBJ, AMF) and converts them into G-code
instructions for FFF printers or PNG layers for mSLA 3D printers. It's
compatible with any modern printer based on the RepRap toolchain, including all
those based on the Marlin, Prusa, Sprinter and Repetier firmware. It also works
with Mach3, LinuxCNC and Machinekit controllers.

PrusaSlicer is based on [Slic3r](https://github.com/Slic3r/Slic3r) by Alessandro Ranellucci and the RepRap community.

See the [project homepage](https://www.prusa3d.com/slic3r-prusa-edition/) and
the [documentation directory](doc/) for more information.

### What language is it written in?

All user facing code is written in C++.
The slicing core is the `libslic3r` library, which can be built and used in a standalone way.
The command line interface is a thin wrapper over `libslic3r`.

### What are PrusaSlicer's main features?

Key features are:

* **multi-platform** (Linux/Mac/Win) and packaged as standalone-app with no dependencies required
* complete **command-line interface** to use it with no GUI
* multi-material **(multiple extruders)** object printing
* multiple G-code flavors supported (RepRap, Makerbot, Mach3, Machinekit etc.)
* ability to plate **multiple objects having distinct print settings**
* **multithread** processing
* **STL auto-repair** (tolerance for broken models)
* wide automated unit testing

Other major features are:

* combine infill every 'n' perimeters layer to speed up printing
* **3D preview** (including multi-material files)
* **multiple layer heights** in a single print
* **spiral vase** mode for bumpless vases
* fine-grained configuration of speed, acceleration, extrusion width
* several infill patterns including honeycomb, spirals, Hilbert curves
* support material, raft, brim, skirt
* **standby temperature** and automatic wiping for multi-extruder printing
* [customizable **G-code macros**](https://github.com/prusa3d/PrusaSlicer/wiki/PrusaSlicer-Macro-Language) and output filename with variable placeholders
* support for **post-processing scripts**
* **cooling logic** controlling fan speed and dynamic print speed

### Development

If you want to compile the source yourself, follow the instructions on one of
these documentation pages:
* [Linux](doc/How%20to%20build%20-%20Linux%20et%20al.md)
* [macOS](doc/How%20to%20build%20-%20Mac%20OS.md)
* [Windows](doc/How%20to%20build%20-%20Windows.md)

### Can I help?

Sure! You can do the following to find things that are available to help with:
* Add an [issue](https://github.com/prusa3d/PrusaSlicer/issues) to the github tracker if it isn't already present.
* Look at [issues labeled "volunteer needed"](https://github.com/prusa3d/PrusaSlicer/issues?utf8=%E2%9C%93&q=is%3Aopen+is%3Aissue+label%3A%22volunteer+needed%22)

### What's PrusaSlicer license?

PrusaSlicer is licensed under the _GNU Affero General Public License, version 3_.
The PrusaSlicer is originally based on Slic3r by Alessandro Ranellucci.

### How can I use PrusaSlicer from the command line?

Please refer to the [Command Line Interface](https://github.com/prusa3d/PrusaSlicer/wiki/Command-Line-Interface) wiki page.
