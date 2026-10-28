import React, {useEffect, useState} from 'react';
import {Box, Text, render, useAnimation, useApp, useInput, usePaste, useWindowSize} from 'ink';

// Never let a piped/non-interactive CI render masquerade as terminal coverage.
// The C++ test starts this process through Session's actual platform PTY.
if (!process.stdin.isTTY || !process.stdout.isTTY || typeof process.stdin.setRawMode !== 'function') {
  throw new Error('The Ink compatibility fixture requires a real input/output TTY');
}

const h = React.createElement;
const choices = ['Aurora', 'Borealis', 'Cirrus'];
const spinner = ['⠋', '⠙', '⠹', '⠸', '⠼', '⠴', '⠦', '⠧'];
const alternateScreen = process.argv.includes('--alternate-screen');
const incrementalRendering = process.argv.includes('--incremental');
const fullscreen = process.argv.includes('--fullscreen');
if (fullscreen && !alternateScreen) {
  throw new Error('--fullscreen requires --alternate-screen');
}

function Workbench() {
  const {exit, waitUntilRenderFlush} = useApp();
  const {columns, rows} = useWindowSize();
  const [selection, setSelection] = useState(0);
  const [input, setInput] = useState('');
  const [pasted, setPasted] = useState('');
  const [complete, setComplete] = useState(false);
  const {frame} = useAnimation({interval: 80, isActive: !complete});
  const progress = (frame % 10) + 1;

  useInput((text, key) => {
    if (key.escape || key.return) {
      setComplete(true);
    } else if (key.upArrow) {
      setSelection(previous => (previous + choices.length - 1) % choices.length);
    } else if (key.downArrow) {
      setSelection(previous => (previous + 1) % choices.length);
    } else if (key.backspace || key.delete) {
      setInput(previous => Array.from(previous).slice(0, -1).join(''));
    } else if (!key.ctrl && !key.meta && !key.tab) {
      setInput(previous => (previous + text).slice(0, 24));
    }
  }, {isActive: !complete});

  usePaste(text => setPasted(text.slice(0, 24)), {isActive: !complete});

  useEffect(() => {
    if (complete) {
      // The final React render must reach stdout before Ink restores terminal
      // modes. No fixed sleeps, raw ANSI frames, or side-channel test protocol.
      void waitUntilRenderFlush().then(() => exit({selection: choices[selection], input, pasted}));
    }
  }, [complete, exit, waitUntilRenderFlush, selection, input, pasted]);

  return h(Box, {
    flexDirection: 'column',
    borderStyle: 'round',
    borderColor: 'cyan',
    paddingX: 1,
    // Let Yoga resolve width synchronously from the resized terminal. A fixed
    // width computed from useWindowSize state can briefly be stale while the
    // resize event is being delivered to React, causing the app itself to wrap.
    width: '100%',
    maxWidth: 72,
    // A genuine full-height layout, not a host escape/erase workaround. Ink's
    // own renderer decides how to redraw this frame when viewport rows change.
    height: fullscreen ? rows : undefined,
  },
  h(Text, {bold: true, color: 'cyan'}, 'StormTTY Ink Workbench'),
  h(Text, null, `Interactive TTY | Size: ${columns}x${rows}`),
  h(Text, null, 'Unicode: café e\u0301 日本 🚀'),
  h(Text, {dimColor: true}, 'Choose a sky with Up/Down; type to edit:'),
  ...choices.map((choice, index) => h(Text, {
    key: choice,
    color: index === selection ? 'green' : undefined,
    bold: index === selection,
  }, `${index === selection ? '❯' : ' '} ${choice}`)),
  h(Text, null, `Selected: ${choices[selection]}`),
  h(Text, null, `Input: <${input}>`),
  h(Text, null, `Paste: <${pasted}>`),
  h(Text, {color: 'yellow'}, `Animation: ${spinner[frame % spinner.length]} [${'█'.repeat(progress)}${'░'.repeat(10 - progress)}] Tick: ${frame}`),
  h(Text, {color: complete ? 'green' : undefined}, complete ? 'Completed cleanly' : 'Enter to finish | Esc to exit'),
  );
}

const app = render(h(Workbench), {
  // Deliberate override for a real PTY hosted inside CI. TTY checks above remain
  // mandatory. debug=false keeps real cursor motion/erase/redraw behavior.
  interactive: true,
  debug: false,
  alternateScreen,
  incrementalRendering,
  maxFps: 30,
});
await app.waitUntilExit();
// Ordinary post-app output also verifies alternate-screen restoration and drain.
console.log('Ink fixture exited cleanly');
