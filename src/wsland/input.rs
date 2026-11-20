use std::path::PathBuf;

use smithay::backend::input::{
    AbsolutePositionEvent, Axis, AxisRelativeDirection, AxisSource, ButtonState, Device,
    DeviceCapability, Event, InputBackend, KeyState, KeyboardKeyEvent, Keycode, PointerAxisEvent,
    PointerButtonEvent, PointerMotionAbsoluteEvent, UnusedEvent,
};


#[derive(Debug)]
pub struct WslandInput;

/// Virtual input device used by the backend to associate input events
#[derive(PartialEq, Eq, Hash, Debug)]
pub struct WslandVirtualDevice;

impl Device for WslandVirtualDevice {
    fn id(&self) -> String {
        String::from("wsland")
    }

    fn name(&self) -> String {
        String::from("wsland virtual input")
    }

    fn has_capability(&self, capability: DeviceCapability) -> bool {
        matches!(
            capability,
            DeviceCapability::Keyboard | DeviceCapability::Pointer
        )
    }

    fn usb_id(&self) -> Option<(u32, u32)> {
        None
    }

    fn syspath(&self) -> Option<PathBuf> {
        None
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct WslandKeyboardInputEvent {
    pub(crate) time: u64,
    pub(crate) state: KeyState,
    pub(crate) count: u32,
    pub(crate) evdev: u32,
}

impl Event<WslandInput> for WslandKeyboardInputEvent {
    fn time(&self) -> u64 {
        self.time
    }

    fn device(&self) -> WslandVirtualDevice {
        WslandVirtualDevice
    }
}

impl KeyboardKeyEvent<WslandInput> for WslandKeyboardInputEvent {
    fn key_code(&self) -> Keycode {
        self.evdev.into()
    }

    fn state(&self) -> KeyState {
        self.state
    }

    fn count(&self) -> u32 {
        self.count
    }
}

#[derive(Debug, Clone)]
pub struct WslandMouseMovedEvent {
    pub(crate) time: u64,
    pub(crate) position_x: f64,
    pub(crate) position_y: f64,
}

impl Event<WslandInput> for WslandMouseMovedEvent {
    fn time(&self) -> u64 {
        self.time
    }

    fn device(&self) -> WslandVirtualDevice {
        WslandVirtualDevice
    }
}

impl PointerMotionAbsoluteEvent<WslandInput> for WslandMouseMovedEvent {}
impl AbsolutePositionEvent<WslandInput> for WslandMouseMovedEvent {
    fn x(&self) -> f64 {
        self.position_x
    }

    fn y(&self) -> f64 {
        self.position_y
    }

    fn x_transformed(&self, _: i32) -> f64 {
        self.x()
    }

    fn y_transformed(&self, _: i32) -> f64 {
        self.y()
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WslandMouseWheelEvent {
    pub(crate) time: u64,
    pub(crate) delta: i32,
    pub(crate) horizontal: bool,
}

impl Event<WslandInput> for WslandMouseWheelEvent {
    fn time(&self) -> u64 {
        self.time
    }

    fn device(&self) -> WslandVirtualDevice {
        WslandVirtualDevice
    }
}

impl PointerAxisEvent<WslandInput> for WslandMouseWheelEvent {
    fn amount(&self, axis: Axis) -> Option<f64> {
        match axis {
            Axis::Vertical => if self.horizontal { Some(0.0) } else { Some(-(self.delta as f64) * 15.0 / 120.0) },
            Axis::Horizontal => if self.horizontal { Some(-(self.delta as f64) * 15.0 / 120.0) } else { Some(0.0) },
        }
    }

    fn amount_v120(&self, axis: Axis) -> Option<f64> {
        match axis {
            Axis::Vertical => if self.horizontal { Some(0.0) } else { Some(-self.delta as f64) },
            Axis::Horizontal => if self.horizontal { Some(-self.delta as f64) } else { Some(0.0) },
        }
    }

    fn source(&self) -> AxisSource {
        AxisSource::Wheel
    }

    fn relative_direction(&self, _axis: Axis) -> AxisRelativeDirection {
        AxisRelativeDirection::Identical
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct WslandMouseInputEvent {
    pub(crate) time: u64,
    pub(crate) button: u32,
    pub(crate) release: bool,
}

impl Event<WslandInput> for WslandMouseInputEvent {
    fn time(&self) -> u64 {
        self.time
    }

    fn device(&self) -> WslandVirtualDevice {
        WslandVirtualDevice
    }
}

impl PointerButtonEvent<WslandInput> for WslandMouseInputEvent {
    fn button_code(&self) -> u32 {
        self.button
    }

    fn state(&self) -> ButtonState {
        if self.release { ButtonState::Released } else { ButtonState::Pressed }
    }
}

impl InputBackend for WslandInput {
    type Device = WslandVirtualDevice;
    type KeyboardKeyEvent = WslandKeyboardInputEvent;
    type PointerAxisEvent = WslandMouseWheelEvent;
    type PointerButtonEvent = WslandMouseInputEvent;
    type PointerMotionEvent = UnusedEvent;
    type PointerMotionAbsoluteEvent = WslandMouseMovedEvent;

    type GestureSwipeBeginEvent = UnusedEvent;
    type GestureSwipeUpdateEvent = UnusedEvent;
    type GestureSwipeEndEvent = UnusedEvent;
    type GesturePinchBeginEvent = UnusedEvent;
    type GesturePinchUpdateEvent = UnusedEvent;
    type GesturePinchEndEvent = UnusedEvent;
    type GestureHoldBeginEvent = UnusedEvent;
    type GestureHoldEndEvent = UnusedEvent;

    type TouchDownEvent = UnusedEvent;
    type TouchUpEvent = UnusedEvent;
    type TouchMotionEvent = UnusedEvent;
    type TouchCancelEvent = UnusedEvent;
    type TouchFrameEvent = UnusedEvent;
    type TabletToolAxisEvent = UnusedEvent;
    type TabletToolProximityEvent = UnusedEvent;
    type TabletToolTipEvent = UnusedEvent;
    type TabletToolButtonEvent = UnusedEvent;

    type SwitchToggleEvent = UnusedEvent;

    type SpecialEvent = UnusedEvent;
}
