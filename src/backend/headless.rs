//! Headless backend for tests.
//!
//! This can eventually grow into a more complete backend if needed, but for now it's missing some
//! crucial parts like dmabufs.

use std::cell::{RefCell};
use std::{env, mem};
use std::os::fd::{RawFd};
use std::rc::Rc;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Context as _;
use calloop::timer::{TimeoutAction, Timer};
use calloop::LoopHandle;
use niri_config::{Config, OutputName};
use smithay::backend::allocator::dmabuf::Dmabuf;
use smithay::backend::allocator::Fourcc;
use smithay::backend::egl::native::EGLSurfacelessDisplay;
use smithay::backend::egl::{EGLContext, EGLDevice, EGLDisplay};
use smithay::backend::input::{KeyState, Keycode};
use smithay::backend::renderer::damage::OutputDamageTracker;
use smithay::backend::renderer::element::RenderElementStates;
use smithay::backend::renderer::gles::{GlesRenderer, GlesTexture};
use smithay::backend::renderer::{Bind, ExportMem, ImportDma, Offscreen};
use smithay::output::{Mode, Output, PhysicalProperties, Subpixel};
use smithay::reexports::wayland_protocols::wp::presentation_time::server::wp_presentation_feedback;
use smithay::utils::{Buffer, Point, Rectangle, Scale, Size, Transform};
use smithay::wayland::dmabuf::{DmabufFeedbackBuilder, DmabufGlobal};
use smithay::wayland::presentation::Refresh;

use super::{IpcOutputMap, OutputId, RenderResult};
use crate::niri::{Niri, RedrawState, State};
use crate::render_helpers::{render_to_vec, resources, shaders, RenderCtx, RenderTarget};
use crate::utils::{get_monotonic_time, logical_output};
use crate::wsland::ffi::CommitState;
use crate::wsland::wsland::Wsland;

pub struct Headless {
    renderer: Option<GlesRenderer>,
    ipc_outputs: Arc<Mutex<IpcOutputMap>>,

    config: Rc<RefCell<Config>>,
    dmabuf_global: Option<DmabufGlobal>,
    use_niri_cursor: bool,
    wsland: Box<Wsland>,
    damage_cursor: Option<Vec<u8>>,
    damage_tracker: Option<OutputDamageTracker>,
    output_texture: Option<GlesTexture>,
    output: Option<Output>,
    damage_age: usize,
}

impl Headless {
    pub fn new(config: Rc<RefCell<Config>>, event_loop: LoopHandle<'static, State>) -> Self {
        Self {
            renderer: None,
            ipc_outputs: Default::default(),

            config,
            dmabuf_global: None,
            use_niri_cursor: false,
            damage_age: 0,
            damage_cursor: None,
            wsland: Box::new(Wsland::new(event_loop)),
            damage_tracker: None,
            output_texture: None,
            output: None,
        }
    }

    pub fn init(&mut self, niri: &mut Niri) {
        self.add_renderer(niri).unwrap();

        self.wsland.start("0.0.0.0", 3389);
    }

    pub fn add_renderer(&mut self, niri: &mut Niri) -> anyhow::Result<()> {
        if self.renderer.is_some() {
            error!("add_renderer: renderer must not already exist");
            return Ok(());
        }

        let mut renderer = unsafe {
            let display =
                EGLDisplay::new(EGLSurfacelessDisplay).context("error creating EGL display")?;
            let context = EGLContext::new(&display).context("error creating EGL context")?;
            GlesRenderer::new(context).context("error creating renderer")?
        };

        resources::init(&mut renderer);
        shaders::init(&mut renderer);

        self.renderer = Some(renderer);

        if env::var_os("USE_DMABUF").is_some() {
            let config = self.config.borrow();
            if let Some(src) = config.animations.window_resize.custom_shader.as_deref() {
                shaders::set_custom_resize_program(self.renderer.as_mut().unwrap(), Some(src));
            }
            if let Some(src) = config.animations.window_close.custom_shader.as_deref() {
                shaders::set_custom_close_program(self.renderer.as_mut().unwrap(), Some(src));
            }
            if let Some(src) = config.animations.window_open.custom_shader.as_deref() {
                shaders::set_custom_open_program(self.renderer.as_mut().unwrap(), Some(src));
            }
            drop(config);

            niri.update_shaders();

            self.create_dmabuf_global(niri);
        }
        Ok(())
    }

    pub fn create_dmabuf_global(&mut self, niri: &mut Niri) {
        let renderer = self.renderer.as_mut().unwrap();

        let default_feedback = || {
            let display = renderer.egl_context().display();
            let device =
                EGLDevice::device_for_display(display).context("error getting EGL device")?;
            let node = device
                .try_get_render_node()
                .context("error getting EGL device render node")?
                .context("failed to query EGL device render node")?;

            let primary_formats = renderer.dmabuf_formats();
            DmabufFeedbackBuilder::new(node.dev_id(), primary_formats)
                .build()
                .context("error building dmabuf feedback")
        };

        // Fallback to dmabuf v3 if we failed to build feedback.
        let dmabuf_global = match default_feedback() {
            Ok(feedback) => niri
                .dmabuf_state
                .create_global_with_default_feedback::<State>(&niri.display_handle, &feedback),
            Err(err) => {
                debug!("failed building default dmabuf feedback, falling back to v3: {err:?}");
                let primary_formats = self.renderer.as_mut().unwrap().dmabuf_formats();
                niri.dmabuf_state
                    .create_global::<State>(&niri.display_handle, primary_formats)
            }
        };
        assert!(self.dmabuf_global.replace(dmabuf_global).is_none());
    }

    pub fn add_output(&mut self, niri: &mut Niri, n: u8, size: (u16, u16)) {
        let connector = Wsland::output_name(n);
        let make = "niri".to_string();
        let model = "wsland".to_string();
        let serial = n.to_string();

        self.output = Some(Output::new(
            connector.clone(),
            PhysicalProperties {
                size: (0, 0).into(),
                subpixel: Subpixel::Unknown,
                make: make.clone(),
                model: model.clone(),
                serial_number: serial.clone(),
            },
        ));

        let mode = Mode {
            size: Size::from((i32::from(size.0), i32::from(size.1))),
            refresh: 60_000,
        };

        let output = self.output.clone().unwrap();
        output.change_current_state(Some(mode), None, None, None);
        output.set_preferred(mode);

        output.user_data().insert_if_missing(|| OutputName {
            connector,
            make: Some(make),
            model: Some(model),
            serial: Some(serial),
        });

        let physical_properties = output.physical_properties();
        self.ipc_outputs.lock().unwrap().insert(
            OutputId::next(),
            niri_ipc::Output {
                name: output.name(),
                make: physical_properties.make,
                model: physical_properties.model,
                serial: None,
                physical_size: None,
                modes: vec![niri_ipc::Mode {
                    width: size.0,
                    height: size.1,
                    refresh_rate: 60_000,
                    is_preferred: true,
                }],
                current_mode: Some(0),
                is_custom_mode: true,
                vrr_supported: false,
                vrr_enabled: false,
                logical: Some(logical_output(&output)),
            },
        );

        self.damage_tracker = Some(OutputDamageTracker::from_output(&output));

        if let Some(renderer) = &mut self.renderer {
            self.output_texture = Some(
                renderer
                    .create_buffer(Fourcc::Argb8888, Size::new(size.0 as i32, size.1 as i32))
                    .unwrap(),
            );

            self.damage_age = 0;
        }

        niri.add_output(output, Some(Duration::from_micros(16_667)), false);
    }

    pub fn remove_output(&mut self, niri: &mut Niri) {
        self.ipc_outputs.lock().unwrap().clear();

        if let Some(output) = self.output.clone() {
            niri.remove_output(&output);
            self.output = None;
        }
    }

    pub fn current_output(&self) -> Option<Output> {
        self.output.clone()
    }

    pub fn seat_name(&self) -> String {
        "wsland".to_owned()
    }

    pub fn with_primary_renderer<T>(
        &mut self,
        f: impl FnOnce(&mut GlesRenderer) -> T,
    ) -> Option<T> {
        self.renderer.as_mut().map(f)
    }

    pub fn render(&mut self, niri: &mut Niri, output: &Output) -> RenderResult {
        if let Some(tracker) = self.damage_tracker.as_mut() {
            if let Some(renderer) = self.renderer.as_mut() {
                if !self.use_niri_cursor {
                    let mut geometry = Rectangle::default();
                    let mut elements = Vec::new();
                    niri.render_cursor(renderer, &output, &mut |rect, elem| {
                        geometry = rect;

                        elements.push(elem);
                    });

                    if !elements.is_empty() {
                        let size = geometry.size;
                        let transform = output.current_transform();
                        let size = transform.transform_size(size);
                        let scale = Scale::from(output.current_scale().fractional_scale());

                        let pixels = render_to_vec(
                            renderer,
                            size,
                            scale,
                            Transform::Flipped180,
                            Fourcc::Argb8888,
                            elements.iter().rev(),
                        ).unwrap();

                        if self.damage_cursor.is_none() || self.damage_cursor.clone().unwrap() != pixels {
                            self.wsland.commit_pointer(&CommitState {
                                x: geometry.loc.x,
                                y: geometry.loc.y,
                                w: geometry.size.w,
                                h: geometry.size.h,
                                opaque: 0,
                                data: pixels.as_ptr().cast(),
                            });

                            self.damage_cursor = Some(pixels);
                        }
                    }
                }

                if let Some(texture) = self.output_texture.as_mut() {
                    let ctx = RenderCtx { renderer, target: RenderTarget::Output, xray: None };
                    let elements = niri.render_to_vec(ctx, output, self.use_niri_cursor);

                    if !elements.is_empty() {
                        let mut framebuffer = renderer.bind(texture).unwrap();

                        let res = tracker
                            .render_output(
                                renderer,
                                &mut framebuffer,
                                self.damage_age,
                                &elements,
                                [0.; 4],
                            )
                            .unwrap();

                        if self.damage_age == 0 { self.damage_age = 1; }
                        niri.update_primary_scanout_output(output, &res.states);

                        if let Some(damages) = res.damage {
                            for rect in damages {
                                let copy_rect = Rectangle::<i32, Buffer>::new(
                                    Point::new(rect.loc.x, rect.loc.y),
                                    Size::new(rect.size.w, rect.size.h),
                                );
                                let mapping = renderer
                                    .copy_framebuffer(&framebuffer, copy_rect, Fourcc::Argb8888)
                                    .unwrap();
                                let pixels = renderer
                                    .map_texture(&mapping)
                                    .unwrap()
                                    .to_vec();

                                self.wsland.commit_output(&CommitState {
                                    x: rect.loc.x,
                                    y: rect.loc.y,
                                    w: rect.size.w,
                                    h: rect.size.h,
                                    opaque: 0,
                                    data: pixels.as_ptr().cast(),
                                });
                            }
                        }
                    }
                }
            }
        }

        let now = get_monotonic_time();
        let states = RenderElementStates::default();
        let mut presentation_feedbacks = niri.take_presentation_feedbacks(output, &states);
        presentation_feedbacks.presented::<_, smithay::utils::Monotonic>(
            now,
            Refresh::Unknown,
            0,
            wp_presentation_feedback::Kind::empty(),
        );

        let output_state = niri.output_state.get_mut(output).unwrap();
        match mem::replace(&mut output_state.redraw_state, RedrawState::Idle) {
            RedrawState::Idle => unreachable!(),
            RedrawState::Queued => (),
            RedrawState::WaitingForVBlank { .. } => unreachable!(),
            RedrawState::WaitingForEstimatedVBlank(token)
            | RedrawState::WaitingForEstimatedVBlankAndQueued(token) => {
                niri.event_loop.remove(token);
            }
        }

        output_state.frame_clock.presented(now);
        output_state.frame_callback_sequence = output_state.frame_callback_sequence.wrapping_add(1);

        let refresh_interval = output_state
            .frame_clock
            .refresh_interval()
            .unwrap_or(Duration::from_micros(16_000));

        let output_clone = output.clone();
        let timer = Timer::from_duration(refresh_interval);
        let token = niri
            .event_loop
            .insert_source(timer, move |_, _, data| {
                let output_state = data.niri.output_state.get_mut(&output_clone).unwrap();
                output_state.frame_callback_sequence =
                    output_state.frame_callback_sequence.wrapping_add(1);

                match mem::replace(&mut output_state.redraw_state, RedrawState::Idle) {
                    RedrawState::WaitingForEstimatedVBlank(_) => (),
                    RedrawState::WaitingForEstimatedVBlankAndQueued(_) => {
                        output_state.redraw_state = RedrawState::Queued;
                        return TimeoutAction::Drop;
                    }
                    _ => unreachable!(),
                }

                if output_state.unfinished_animations_remain {
                    data.niri.queue_redraw(&output_clone);
                } else {
                    data.niri.send_frame_callbacks(&output_clone);
                }
                TimeoutAction::Drop
            })
            .unwrap();
        output_state.redraw_state = RedrawState::WaitingForEstimatedVBlank(token);
        // FIXME: request redraw on unfinished animations remain

        RenderResult::Submitted
    }

    pub fn import_dmabuf(&mut self, _dmabuf: &Dmabuf) -> bool {
        unimplemented!()
    }

    pub fn ipc_outputs(&self) -> Arc<Mutex<IpcOutputMap>> {
        self.ipc_outputs.clone()
    }

    pub fn update_key_count(&mut self, keycode: Keycode, state: KeyState) -> u32 {
        self.wsland.update_key_count(keycode, state)
    }

    pub fn wsland_clipboard_callback(&mut self, op_id: &u32, mime_type: String, fd: RawFd) {
        self.wsland.clipboard_callback(op_id, mime_type, fd);
    }

    pub fn wsland_clipboard_clear(&mut self) {
        self.wsland.clipboard_clear();
    }

    pub fn wsland_clipboard_format_list(&mut self, mime_types: Vec<String>) {
        self.wsland.clipboard_format_list(mime_types);
    }

    pub fn switch_next_monitor(&mut self) {
        self.wsland.switch_next_monitor();
    }
}

/*impl Default for Headless {
    fn default() -> Self {
        Self::new()
    }
}*/
