

#[repr(transparent)]
pub struct Vio (MYSQL_PLUGIN_VIO);

#[repr(transparent)]
pub struct VioInfo(MYSQL_PLUGIN_VIO_INFO);

pub struct WriteFailure;

impl Vio {
    fn read_packet(&self) {
        let read_fn = self.0.read_packet.expect("read function is null!");
    }

    fn write_packet(&self, packet: &[u8]) -> Result<(), WriteFailure> {
        let write_fn = self.0.write_packet.expect("write function is null!");
        let res = unsafe { write_fn(&self, packet.as_ptr(), packet.len()) };
        if res == 0 {
            Ok(())
        } else {
            Err(WriteFailure)
        }
    }

    fn info(&self) -> VioInfo {
        let info_fn = self.0.write_packet.expect("write function is null!");
        let vi: MaybeUninit<VioInfo>::zeroed();
        unsafe {
            info_fn(&self, &vi);
            vi.assume_init();
        }
        vi
    }
}
