use std::collections::HashSet;
use std::ffi::{CStr, CString};
use std::ops::Div;
use std::os::fd::{BorrowedFd, FromRawFd, OwnedFd, RawFd};
use std::os::raw;
use std::os::raw::c_void;
use std::process::exit;
use std::ptr::null_mut;
use std::slice::from_raw_parts;
use std::str::FromStr;
use std::sync::{Arc, Mutex};

use calloop::generic::Generic;
use calloop::{Interest, LoopHandle, Mode, PostAction};
use data_device::set_data_device_selection;
use raw::c_char;
use smithay::backend::input::{InputEvent, KeyState, Keycode};
use smithay::wayland::selection::data_device::request_data_device_client_selection;
use smithay::wayland::selection::{data_device, SelectionHandler, SelectionTarget};

use crate::handlers::SelectionUserDataType;
use crate::niri::State;
use crate::utils::get_monotonic_time;
use crate::wsland::ffi::{windows_provide_format_request, windows_sync_format, wsland_server_commit_output, wsland_server_commit_pointer, wsland_server_create, wsland_server_destroy, wsland_server_switch_next_monitor, CommitState, DispatchEventInvoke, WslandCallback, WslandServer, KEY_CAPSLOCK};
use crate::wsland::input::{
    WslandInput, WslandKeyboardInputEvent, WslandMouseInputEvent, WslandMouseMovedEvent,
    WslandMouseWheelEvent,
};

pub struct Wsland {
    event_loop: LoopHandle<'static, State>,
    pressed_keys: Arc<Mutex<HashSet<Keycode>>>,

    server: *mut WslandServer,
    callback: WslandCallback,
}

impl Drop for Wsland {
    fn drop(&mut self) {
        if !self.server.is_null() {
            unsafe {
                wsland_server_destroy(self.server);
            }
            self.server = null_mut();
        }
    }
}

impl Wsland {
    pub fn new(event_loop: LoopHandle<'static, State>) -> Self {
        Self {
            event_loop,
            pressed_keys: Arc::new(Mutex::new(HashSet::new())),

            server: null_mut(),
            callback: WslandCallback::new(),
        }
    }

    pub fn output_name(n: u8) -> String {
        format!("wsland-{n}")
    }

    pub fn update_key_count(&mut self, keycode: Keycode, state: KeyState) -> u32 {
        match state {
            KeyState::Pressed => {
                self.pressed_keys.lock().unwrap().insert(keycode);
            }
            KeyState::Released => {
                self.pressed_keys.lock().unwrap().remove(&keycode);
            }
        }

        self.pressed_keys.lock().unwrap().len() as u32
    }

    pub fn start(&mut self, addr: &str, port: u16) {
        let w = self as *mut Wsland;
        let s = CString::from_str(addr).unwrap();

        unsafe {
            self.server = wsland_server_create(port, s.as_ptr(), &self.callback, w.cast());

            if self.server.is_null() {
                error!("wsland start: wsland_server_create failed!");
                exit(0);
            }
        }
    }

    pub fn commit_pointer(&mut self, state: &CommitState) {
        unsafe {
            wsland_server_commit_pointer(self.server, state as *const CommitState);
        }
    }

    pub fn commit_output(&mut self, state: &CommitState) {
        unsafe {
            wsland_server_commit_output(self.server, state as *const CommitState);
        }
    }

    pub fn clipboard_callback(&mut self, op_id: &u32, mime_type: String, fd: RawFd) {
        let c_mime_type = CString::from_str(mime_type.as_str()).unwrap();

        unsafe {
            windows_provide_format_request(self.server, *op_id, c_mime_type.as_ptr(), fd);
        }
    }

    pub fn clipboard_clear(&mut self) {
        // todo notify clear
    }

    pub fn switch_next_monitor(&mut self) {
        unsafe {
            wsland_server_switch_next_monitor(self.server);
        }
    }

    pub fn clipboard_format_list(&mut self, mime_types: Vec<String>) {
        let c_strings: Vec<CString> = mime_types
            .iter()
            .map(|v| CString::from_str(v.as_str()).unwrap())
            .collect();

        unsafe {
            let c_ptrs: Vec<*const c_char> = c_strings.iter().map(|v| v.as_ptr().cast()).collect();
            windows_sync_format(self.server, c_ptrs.as_ptr(), c_ptrs.len());
        }
    }
}

impl WslandCallback {
    pub fn new() -> Self {
        Self {
            dispatch_event: Some(WslandCallback::dispatch_event),

            mouse_motion: Some(WslandCallback::mouse_motion),
            mouse_button: Some(WslandCallback::mouse_button),
            mouse_axis: Some(WslandCallback::mouse_axis),

            keyboard_key: Some(WslandCallback::keyboard_key),
            keyboard_reset: Some(WslandCallback::keyboard_reset),

            output_create: Some(WslandCallback::output_create),
            output_destroy: Some(WslandCallback::output_destroy),

            windows_provide_format: Some(WslandCallback::windows_provide_format),
            windows_provide_format_data: Some(WslandCallback::windows_provide_format_data),
            windows_sync_format_request_write: Some(
                WslandCallback::windows_sync_format_request_write,
            ),
        }
    }

    pub unsafe extern "C" fn dispatch_event(
        ctx: *mut c_void,
        fd: i32,
        write: bool,
        data: *mut c_void,
        invoke: DispatchEventInvoke,
    ) {
        let wsland = &mut *(ctx as *mut Wsland);

        let interest = if write {
            Interest::WRITE
        } else {
            Interest::READ
        };

        wsland
            .event_loop
            .insert_source(
                Generic::new(BorrowedFd::borrow_raw(fd), interest, Mode::Level),
                move |_, _, state| {
                    if invoke.unwrap()(data, (state as *mut State).cast()) == 0 {
                        return Ok(PostAction::Remove);
                    }

                    Ok(PostAction::Continue)
                },
            )
            .unwrap();
    }

    pub unsafe extern "C" fn mouse_motion(ptr: *mut c_void, x: f64, y: f64) {
        let state = &mut *(ptr as *mut State);

        if let Some(output) = state.backend.headless().current_output() {
            let output_scale = output.current_scale().fractional_scale();

            state.process_input_event(InputEvent::<WslandInput>::PointerMotionAbsolute {
                event: WslandMouseMovedEvent {
                    time: get_monotonic_time().as_micros() as u64,
                    position_y: y.div(output_scale),
                    position_x: x.div(output_scale),
                },
            });
        }
    }

    pub unsafe extern "C" fn mouse_button(ptr: *mut c_void, key: u32, release: bool) {
        let state = &mut *(ptr as *mut State);

        state.process_input_event(InputEvent::<WslandInput>::PointerButton {
            event: WslandMouseInputEvent {
                time: get_monotonic_time().as_micros() as u64,
                button: key,
                release,
            },
        });
    }

    pub unsafe extern "C" fn mouse_axis(ptr: *mut c_void, horizontal: bool, delta: i32) {
        let state = &mut *(ptr as *mut State);

        state.process_input_event(InputEvent::<WslandInput>::PointerAxis {
            event: WslandMouseWheelEvent {
                time: get_monotonic_time().as_micros() as u64,
                horizontal,
                delta,
            },
        });
    }

    pub unsafe extern "C" fn keyboard_key(ptr: *mut c_void, evdev: u32, release: bool) {
        let state = &mut *(ptr as *mut State);

        let keycode = Keycode::from(evdev);
        let key_state = if release {
            KeyState::Released
        } else {
            KeyState::Pressed
        };
        let count = state
            .backend
            .headless()
            .update_key_count(keycode, key_state);

        state.process_input_event(InputEvent::<WslandInput>::Keyboard {
            event: WslandKeyboardInputEvent {
                time: get_monotonic_time().as_micros() as u64,
                state: key_state,
                evdev,
                count,
            },
        });
    }

    pub unsafe extern "C" fn keyboard_reset(
        ptr: *mut c_void,
        keys: *const u32,
        count: usize,
        caps_lock: bool,
    ) {
        let state = &mut *(ptr as *mut State);
        let keyboard = state.niri.seat.get_keyboard().unwrap();

        for evdev_ptr in from_raw_parts(keys, count) {
            let evdev = *evdev_ptr;
            let keycode = Keycode::from(evdev);
            let key_state = KeyState::Released;

            let count = state
                .backend
                .headless()
                .update_key_count(keycode, key_state);

            state.process_input_event(InputEvent::<WslandInput>::Keyboard {
                event: WslandKeyboardInputEvent {
                    time: get_monotonic_time().as_micros() as u64,
                    state: key_state,
                    evdev,
                    count,
                },
            });
        }

        if keyboard.modifier_state().caps_lock != caps_lock {
            let evdev = KEY_CAPSLOCK + 8;
            let keycode = Keycode::from(evdev);
            let key_state = KeyState::Pressed;

            let count = state
                .backend
                .headless()
                .update_key_count(keycode, key_state);

            state.process_input_event(InputEvent::<WslandInput>::Keyboard {
                event: WslandKeyboardInputEvent {
                    time: get_monotonic_time().as_micros() as u64,
                    state: key_state,
                    evdev,
                    count,
                },
            });

            let keycode = Keycode::from(evdev);
            let key_state = KeyState::Released;

            let count = state
                .backend
                .headless()
                .update_key_count(keycode, key_state);

            state.process_input_event(InputEvent::<WslandInput>::Keyboard {
                event: WslandKeyboardInputEvent {
                    time: get_monotonic_time().as_micros() as u64,
                    state: key_state,
                    evdev,
                    count,
                },
            });
        }
    }

    pub unsafe extern "C" fn windows_provide_format(
        ptr: *mut c_void,
        op_id: u32,
        types: *const *const c_char,
        count: usize,
    ) {
        let state = &mut *(ptr as *mut State);

        let mut mime_types = Vec::<String>::with_capacity(count);
        for mime_type in from_raw_parts(types, count) {
            mime_types.push(CStr::from_ptr(*mime_type).to_string_lossy().into_owned())
        }

        set_data_device_selection::<State>(
            &state.niri.display_handle,
            &state.niri.seat,
            mime_types.clone(),
            Arc::new(SelectionUserDataType::Remote { op_id, mime_types }),
        );
    }

    pub unsafe extern "C" fn windows_provide_format_data(
        ptr: *mut c_void,
        fd: i32,
        type_ptr: *const c_char,
        data_ptr: *const u8,
        count: usize,
    ) {
        let state = &mut *(ptr as *mut State);
        let data_vec = from_raw_parts(data_ptr, count).to_vec();
        let mime_type = CStr::from_ptr(type_ptr).to_string_lossy().into_owned();

        state.send_selection(
            SelectionTarget::Clipboard,
            mime_type,
            OwnedFd::from_raw_fd(fd),
            state.niri.seat.clone(),
            &Arc::new(SelectionUserDataType::Local(data_vec)),
        );
    }

    pub unsafe extern "C" fn windows_sync_format_request_write(
        ptr: *mut c_void,
        mime_type_ptr: *const c_char,
        fd: i32,
    ) -> bool {
        let state = &mut *(ptr as *mut State);
        let mime_type = CStr::from_ptr(mime_type_ptr).to_string_lossy().into_owned();

        request_data_device_client_selection(&state.niri.seat, mime_type, OwnedFd::from_raw_fd(fd))
            .is_ok()
    }

    pub unsafe extern "C" fn output_create(ptr: *mut c_void, w: u16, h: u16) {
        let state = &mut *(ptr as *mut State);

        state
            .backend
            .headless()
            .add_output(&mut state.niri, 1, (w, h));
    }

    pub unsafe extern "C" fn output_destroy(state_ptr: *mut c_void) {
        let state = &mut *(state_ptr as *mut State);

        state.backend.headless().remove_output(&mut state.niri);
    }
}
