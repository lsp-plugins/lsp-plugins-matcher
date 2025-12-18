<?php
	plugin_header();
	$m      =   (strpos($PAGE, '_mono') > 0) ? 'm' : 's';
	$sc     =   (strpos($PAGE, 'sc_') === 0);
?>

<p>
	This plugin allows to introduce the spectral matching to the desired reference track into your track!
	Bring the sound of your favourite mix into your mix!
</p>
<p>
	The main working principle is simple and based on two profiles. The first profile is the profile of your track
	which is respresented as a frequency spectrum. The second profile is the profile of the reference track
	which also is represented as a frequency spectrum. When you have both valid profiles, the plugin automatically
	computes the frequency response to match your track to the reference one. 
</p>
<p>
	To get the profile of the input track, you need to capture the input signal by the plugin. To get the profile of the
	reference track, you can capture the input signal by the plugin or use shared memory link or sidechain input (for plugin
	versions where it is present). Even more, you can load a separate file and obtain the profile from it or draw your
	frequency response envelope using the 10-band equalizer.
</p>
<p>
	But that's not all! You can also perform dynamic matching by turning on continuous input track profile capturing,
	continuous reference track capturing or both!
</p>
<p>
    The final correction curve may become very aggressive so you also can control the maximum gain adjustment range.
    And for the dynamic matching case you also can control the morphing speed.
</p>

<p><b>Controls</b> section:</p>
<ul>
	<li><b>Input Profile</b> - the sub-section that allows to capture and configure profile of the input track:</li>
	<ul>
		<li><b>Morph time</b> - the profile morph time for the dynamic input profile. The resulting dynamic profile is
		smoothed by a filter: the lower value is, the faster dynamic profile is changing.
		<li><b>Record</b> - the button that starts/stops recording of the static input profile.</li>
		<li><b>Ready</b> - the LED that indicates that static input profile has been sucessfully recorded.</li>
	</ul>
	<li><b>Capture Profile</b> - The sub-section that allows to capture and configure static profile of the reference track:</li>
	<ul>
		<li><b>Source</b> - the combo box that allows to select audio channel for capturing profile data:</li>
		<ul>
			<li><b>Input</b> - the input audio channel is used for capturing.</li>
			<?php if ($sc) { ?>
			<li><b>Sidechain</b> - the sidechain audio channel is used for capturing.</li>
			<?php } ?>
			<li><b>Link</b> - the shared memory link audio channel is used for capturing.</li>
		</ul>
		<li><b>Record</b> - the button that starts/stops recording of the static input profile.</li>
		<li><b>Listen</b> - button that allows to pass the selected audio channel to the audio output of the plugin.</li>
		<li><b>Ready</b> - the LED that indicates that static capture profile has been sucessfully recorded.</li>
	</ul>
	<li><b>Link</b> - the name of the shared memory link to pass sidechain signal.</li>
	<li><b>External</b> - the sub-section that allows to control the parameters of the dynamic reference profile when
		dynamic refrence profile is enabled</li>
	<ul>
		<li><b>Morph time</b> - the profile morph time for the dynamic input profile. The resulting dynamic profile is
		smoothed by a filter: the lower value is, the faster dynamic profile is changing.
	</ul>
	<li><b>File</b> - the button that shows the file loading overlay.</li>
	<li><b>Ready</b> - the LED that indicates that file profile has been sucessfully processed.</li>
	<li><b>Envelope</b> - the button that shows the overlay with custom equalization envelope controls.</li>
	<li><b>Limits</b> - the button that shows the matching profile limits.</li>
	<li><b>Enable</b> - the button that enables the limits for matching profile.</li>
	<li><b>Morphing</b> - the button that shows the overlay with dynamic profile morphing timings.</li>
	<li><b>Match</b> - perform immediate matching between input and reference profile without morphing stage.</li>
	<li><b>Filters</b> - the button that shows the overlay with additional post-processing filters.</li>	
</ul>

<p><b>'Signal' section:</b></p>
<ul>
	<li><b>Input</b> - overall input gain.</li>
	<li><b>Output</b> - overall output gain.</li>
</ul>

<p><b>'Analysis' section:</b></p>
<ul>
	<li><b>Show</b> - the button that enables/disables display of all curves in the section on the frequency graph.</li>
	<li><b>Input</b> - enables drawing of FFT spectrum of the input signal.</li>
	<li><b>Output</b> - enables drawing of FFT spectrum of the output signal.</li>
	<li><b>Source</b> - enables drawing of FFT spectrum of the signal passed to the capture profile.</li>
	<li><b>Reference</b> - enables drawing of FFT spectrum of the reference profile.</li>
	<li><b>Reactivity</b> - the reactivity of the FFT graph.</li>
	<li><b>Shift</b> - additional gain shift of the FFT graph.</li>
</ul>

<p><b>'Profiles' section:</b></p>
<ul>
	<li><b>Show</b> - the button that enables/disables display of all profiles in the section on the frequency graph.</li>
	<li><b>Input (Static profiles)</b> - enables drawing of the static input profile.</li>
	<li><b>Capture</b> - enables drawing of the captured profile.</li>
	<li><b>File</b> - enables drawing of the profile loaded from the file.</li>
	<li><b>Envelope</b> - enables drawing of the profile built from envelope.</li>
	<li><b>Input (Dynamic profiles)</b> - enables drawing of the dynamic input profile.</li>
	<li><b>External</b> - enables drawing of the dynamic profile from the external channel.</li>
</ul>

<p><b>'Result' section:</b></p>
<ul>
	<li><b>Match</b> - Enabled drawing of the matching (resulting) profile.</li>
</ul>

<p><b>Other controls:</b></p>
<ul>
	<li><b>FFT Frame</b> - the size of the FFT frame used for processing. The more value is, the more precise low-end processing will be but
	the more latency will be introduced to the original signal.</li>
	<li><b>Input</b> - the combo box that allows to select the input profile.</li>
	<ul>
		<li><b>Static</b> - the static input profile is used which should be captured first.</li>
		<li><b>Dynamic</b> - the dynamic input profile is used which is permanently recorded and updated.</li>
	</ul>
	<li><b>Reference</b> - the combo box that allows to select the reference profile.</li>
	<ul>
		<li><b>None</b> - no reference profile is used, the plugin will use the identity profile which will not affect the sound.</li>
		<li><b>Capture</b> - the capture profile is used which should be captured first.</li>
		<li><b>Capture</b> - the capture profile is used which should be captured first.</li>
	</ul>
	
	<li><b>File</b> - enables drawing of the profile loaded from the file.</li>
	<li><b>Envelope</b> - enables drawing of the profile built from envelope.</li>
	<li><b>Input (Dynamic profiles)</b> - enables drawing of the dynamic input profile.</li>
	<li><b>File</b> - the static profile built from file is used.</li>
	<li><b>Envelope</b> - the static envelope profile is used.</li>
	<?php if ($sc) { ?>
	<li><b>Sidechain</b> - the dynamic external profile is used  based on the sidechain input.</li>
	<?php } ?>
	<li><b>Link</b> - the dynamic external profile is used based on the input of the shared memory link.</li>

	<li><b>Blend</b> - the mix proportion between the reference profile and the input profile before the matching profile
	is computed.</li>
	<?php if ($m == 's') { ?>
	<li><b>Stereo link</b> - the stereo link between the left channel and right channel of the matching profile.</li>
	<?php } ?>
	
	<li><b>Save</b> - the button that allows to save the impulse response of the matching profile to the separate audio file.</li>
</ul>
	
<p><b>File loading overlay:</b></p>
<ul>
	<li><b>Pitch</b> - the relative file pitch in semitones.</li>
	<li><b>Head cut</b> - the time to be cut from the beginning of the file.</li>
	<li><b>Tail cut</b> - the time to be cut from the end of the file.</li>
	<li><b>Listen</b> - the button that plays the preview of the file.</li>
	<li><b>Stop</b> - the button that stops the preview of the file.</li>
</ul>

<p><b>Envelope overlay:</b></p>
<ul>
	<li><b>Faders</b> - the faders that allow to control the gain of the corresponding frequency band for buildin the custom envelope.</li>
</ul>

<p><b>Limits overlay:</b></p>
<ul>
	<li><b>Top</b> - the button that enables the limiting of the matching profile on the top.</li>
	<li><b>Bottom</b> - the button that enables the limiting of the matching profile on the bottom.</li>
	<li><b>Enable</b> - the button that enables the limits for matching profile.</li>	
	<li><b>Faders</b> - the faders that allow to control the top and bottom limits of the corresponding frequency band.</li>
</ul>

<p><b>Morphing overlay:</b></p>
<ul>
	<li><b>Link</b> - the button that allows to simultaneously edit all timings with one single fader.</li>
	<li><b>Match</b> - perform immediate matching between input and reference profile without morphing stage.</li>
	<li><b>Enable</b> - the button that enables the limits for matching profile.</li>	
	<li><b>Faders</b> - the faders that allow to control the morphing time of the corresponding frequency band.</li>
</ul>

<p><b>Filters overlay:</b></p>
<ul>
	<li><b>HPF</b> - the button that enables high-pass filter.</li>
	<li><b>HPF Freq</b> - the cut-off frequency of the high-pass filter.</li>
	<li><b>HPF Slope</b> - the slope of the high-pass filter.</li>
	<li><b>LPF</b> - the button that enables low-pass filter.</li>
	<li><b>LPF Freq</b> - the cut-off frequency of the low-pass filter.</li>
	<li><b>LPF Slope</b> - the slope of the low-pass filter.</li>
	<li><b>Clip</b> - the slope that enables brick-wall clipping filter at the specified frequency.</li>
	<li><b>Clip Freq</b> - the cut-off frequency of the brick-wall clipping filter.</li>	
</ul>


