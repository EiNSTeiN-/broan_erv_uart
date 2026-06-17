#include "broan.h"

#ifdef USE_ESP32
#include "esphome/components/uart/uart_component_esp_idf.h"
#include <driver/uart.h>
#endif

namespace esphome {
namespace broan { // Change 'broan' to match your component name

static const uint32_t UART_DIAGNOSTIC_BAUD_RATES[] = { 9600, 19200, 38400, 57600, 115200 };
static constexpr uint32_t UART_DIAGNOSTIC_START_DELAY_MS = 30000;
static constexpr uint32_t UART_DIAGNOSTIC_ATTEMPT_MS = 10000;
static constexpr uint32_t UART_DIAGNOSTIC_BURST_IDLE_MS = 50;
static constexpr size_t UART_DIAGNOSTIC_MAX_BURSTS = 16;
static constexpr size_t UART_DIAGNOSTIC_MAX_BURST_BYTES = 128;
static constexpr size_t UART_DIAGNOSTIC_MAX_VALID_FRAMES = 8;
static constexpr size_t UART_DIAGNOSTIC_MAX_BYTES_PER_LOOP = 512;
static constexpr uint32_t UART_DIAGNOSTIC_PASS_THROUGH_REPORT_MS = 10000;
static constexpr uint32_t FAN_MODE_OPTIMISTIC_HOLD_MS = 10000;


void BroanComponent::setup()
{
	//uart::UARTDevice::setup();
	Component::setup();
	//esp_log_level_set("broan", ESP_LOG_DEBUG);

	m_vecHeader.resize(5);
	m_HrvFrameReader.m_vecBuffer.reserve(5 + MAX_FRAME_PAYLOAD_SIZE + 2);
	m_RemoteFrameReader.m_vecBuffer.reserve(5 + MAX_FRAME_PAYLOAD_SIZE + 2);

	for( int i=0; i<BroanField::MAX_FIELDS; i++ )
		m_vecFields[i].markDirty();

  	if(flow_control_pin_)
    	this->flow_control_pin_->setup();

	if(remote_flow_control_pin_)
    	this->remote_flow_control_pin_->setup();

	m_unUartDiagnosticBootAt = millis();

	if( m_bUartDiagnosticMode )
	{
		ESP_LOGW("broan", "Broan UART diagnostic mode enabled. Normal HRV control and pass-through are disabled.");
		ESP_LOGW("broan", "UART diagnostic will start automatically in %ums.", UART_DIAGNOSTIC_START_DELAY_MS);
	}
}


void BroanComponent::loop()
{
	if( m_bUartDiagnosticMode )
	{
		processUartDiagnostic();
		return;
	}

	if( isPassThroughEnabled() )
	{
		processPassThrough();
	}
	else
	{
		while ( true )
		{
			if( !readHeader() ) break;
			bool bRead = readMessage();
			if( !bRead ) break;
		}
	}

	replyIfAllowed();

	runTasks();
}

void BroanComponent::dump_config()
{
	ESP_LOGCONFIG("broan", "Broan:");
	ESP_LOGCONFIG("broan", "Pass-through mode: %s", isPassThroughEnabled() ? "enabled" : "disabled" );
	ESP_LOGCONFIG("broan", "UART diagnostic mode: %s", m_bUartDiagnosticMode ? "enabled" : "disabled" );
	ESP_LOGCONFIG("broan", "Server address: 0x%02X", m_nServerAddress );
	ESP_LOGCONFIG("broan", "Client address: 0x%02X", m_nClientAddress );
	if(flow_control_pin_)
	{
		char buffer[255];
		this->flow_control_pin_->dump_summary( buffer, 255 );
		ESP_LOGCONFIG("broan", "Flow Control Pin: %s", buffer );
	}
	if(remote_flow_control_pin_)
	{
		char buffer[255];
		this->remote_flow_control_pin_->dump_summary( buffer, 255 );
		ESP_LOGCONFIG("broan", "Remote Flow Control Pin: %s", buffer );
	}
}

float BroanComponent::get_setup_priority() const
{
  // After UART bus
  return setup_priority::BUS - 1.0f;
}

bool BroanComponent::readHeader()
{
	if( m_bHaveHeader )
	{
		//ESP_LOGD("broan", "Recycling header (good)");
		return true;
	}

	if( available() < 5 )
		return false;

	for (uint8_t i = 0; i < 5; i++) {
		m_vecHeader[i] = read();
		if( i == 0 && m_vecHeader[i] != 0x01 )
		{
			ESP_LOGW("broan", "Alignment: Unexpected %02X in position %i", m_vecHeader[i], i);
			return false;
		}

		if( i == 3 && m_vecHeader[i] != 0x01 )
		{
			ESP_LOGW("broan", "Alignment: Unexpected %02X in position %i", m_vecHeader[i], i);
			return false;
		}
	}

	uint8_t head = m_vecHeader[0];
	if ( m_vecHeader[1] > 32 || m_vecHeader[2] > 32 )
	{
		ESP_LOGW("broan", "Alignment: Unexpected %02X %02X %02X %02X %02X",
			m_vecHeader[0], m_vecHeader[1], m_vecHeader[2], m_vecHeader[3], m_vecHeader[4]);
		return false;
	}

	m_bHaveHeader = true;

	return true;
}

void BroanComponent::writeRegisters( const std::vector<BroanField_t> &values )
{
	std::vector<uint8_t> message;

	message.push_back(0x40); // Write

	for( BroanField_t value : values )
	{
		message.push_back( value.m_nOpcodeHigh );
		message.push_back( value.m_nOpcodeLow );
		uint8_t len = value.m_nType == BroanFieldType::Byte ? 0x01 : 0x04;
		message.push_back( len );
		for( int i=0; i<len; i++ )
			message.push_back( value.m_value.m_rgBytes[i] );
	}

	queueMessage( message );
}

bool BroanComponent::readMessage()
{
	uint8_t target = m_vecHeader[1];
	uint8_t sender = m_vecHeader[2];
	int len = m_vecHeader[4];

	if( !m_bHaveHeader )
		return false;

	if( available() < len + 2 )
	{
		//ESP_LOGD("broan", "Waiting for rest of packet to show up in buffer (Want %i have %i)", len + 2, available() );
		return false;
	}

	m_bHaveHeader = false;

	std::vector<uint8_t> message(len);

	for (uint8_t i = 0; i < len; i++)
	{
		if (!available())
		{
			ESP_LOGE("broan", "Exhausted ring buffer somehow");
			return false;
		}

		message[i] = read();
	}

	uint8_t checksum = read();
	uint8_t expected_checksum = calculateChecksum(sender, target, message);
	if (checksum != expected_checksum)
	{
		ESP_LOGE("broan", "Checksum mismatch: got %02X, expected %02X", checksum, expected_checksum);
		return false;
	}

	uint8_t footer = read();
	if (footer != 0x04)
	{
		ESP_LOGE("broan", "Missing 0x04 footer, incomplete read??");
		return false;
	}

	handleMessage(sender, target, message);

	return true;
}

bool BroanComponent::readFrame(uart::UARTComponent *uart, BroanFrameReader &reader, BroanFrame &frame, const char *label)
{
	while( uart && uart->available() > 0 )
	{
		uint8_t value;
		if( !uart->read_byte( &value ) )
			return false;

		if( reader.m_vecBuffer.empty() && value != 0x01 )
		{
			ESP_LOGW("broan", "%s alignment: unexpected %02X in position 0", label, value );
			continue;
		}

		reader.m_vecBuffer.push_back( value );

		size_t pos = reader.m_vecBuffer.size() - 1;
		if( ( pos == 1 || pos == 2 ) && value > 32 )
		{
			ESP_LOGW("broan", "%s alignment: unexpected address %02X in position %i", label, value, static_cast<int>(pos) );
			reader.m_vecBuffer.clear();
			continue;
		}
		if( pos == 3 && value != 0x01 )
		{
			ESP_LOGW("broan", "%s alignment: unexpected %02X in position %i", label, value, static_cast<int>(pos) );
			reader.m_vecBuffer.clear();
			continue;
		}

		if( reader.m_vecBuffer.size() < 5 )
			continue;

		size_t len = reader.m_vecBuffer[4];
		size_t total = 5 + len + 2;
		if( reader.m_vecBuffer.size() < total )
			continue;

		if( reader.m_vecBuffer[total - 1] != 0x04 )
		{
			ESP_LOGE("broan", "%s missing 0x04 footer", label );
			reader.m_vecBuffer.clear();
			continue;
		}

		std::vector<uint8_t> message(
			reader.m_vecBuffer.begin() + 5,
			reader.m_vecBuffer.begin() + 5 + len
		);

		uint8_t checksum = reader.m_vecBuffer[5 + len];
		uint8_t expected_checksum = calculateChecksum( reader.m_vecBuffer[2], reader.m_vecBuffer[1], message );
		if( checksum != expected_checksum )
		{
			ESP_LOGE("broan", "%s checksum mismatch: got %02X, expected %02X", label, checksum, expected_checksum );
			reader.m_vecBuffer.clear();
			continue;
		}

		frame.m_nTarget = reader.m_vecBuffer[1];
		frame.m_nSender = reader.m_vecBuffer[2];
		frame.m_vecMessage = message;
		frame.m_vecRaw = reader.m_vecBuffer;

		reader.m_vecBuffer.clear();
		return true;
	}

	return false;
}

bool BroanComponent::readFrameFromHrv(BroanFrame &frame)
{
	return readFrame( this->parent_, m_HrvFrameReader, frame, "HRV" );
}

bool BroanComponent::readFrameFromRemote(BroanFrame &frame)
{
	return readFrame( this->remote_uart_, m_RemoteFrameReader, frame, "remote" );
}

void BroanComponent::processPassThrough()
{
	int processed = 0;
	while( processed < 16 )
	{
		bool handled = false;

		BroanFrame hrvFrame;
		if( readFrameFromHrv( hrvFrame ) )
		{
			handleHrvPassThroughFrame( hrvFrame );
			handled = true;
			processed++;
		}

		BroanFrame remoteFrame;
		if( readFrameFromRemote( remoteFrame ) )
		{
			handleRemotePassThroughFrame( remoteFrame );
			handled = true;
			processed++;
		}

		if( !handled )
			break;
	}
}

bool BroanComponent::shouldTakePrivateGrant(const BroanFrame &frame) const
{
	return isPassThroughEnabled()
		&& !m_bPrivateControlSession
		&& !m_bHaveControl
		&& !m_bExpectingReply
		&& !m_vecSendQueue.empty()
		&& frame.m_nTarget == m_nClientAddress
		&& !frame.m_vecMessage.empty()
		&& frame.m_vecMessage[0] == 0x04;
}

void BroanComponent::handleHrvPassThroughFrame(const BroanFrame &frame)
{
	if( frame.m_vecMessage.empty() )
	{
		writeRawToRemote( frame.m_vecRaw );
		return;
	}

	if( m_bPrivateControlSession )
	{
		handleMessage( frame.m_nSender, frame.m_nTarget, frame.m_vecMessage );
		return;
	}

	if( shouldTakePrivateGrant( frame ) )
	{
		m_bPrivateControlSession = true;
		handleMessage( frame.m_nSender, frame.m_nTarget, frame.m_vecMessage );
		return;
	}

	writeRawToRemote( frame.m_vecRaw );

	if( frame.m_nTarget != m_nClientAddress )
		return;

	switch( frame.m_vecMessage[0] )
	{
		case 0x02:
			m_bERVReady = true;
			break;

		case 0x04:
			m_nLastHadControl = millis();
			m_bERVReady = true;
			break;

		case 0x21:
			parseBroanFields( frame.m_vecMessage );
			m_bERVReady = true;
			break;

		case 0x41:
		{
			for( size_t i=1; i + 1 < frame.m_vecMessage.size(); i+=2 )
			{
				BroanField_t *pField = lookupField( frame.m_vecMessage[i], frame.m_vecMessage[i+1] );
				if( !pField )
				{
					ESP_LOGW("broan", "Got write response for unknown field %02X %02X", frame.m_vecMessage[i], frame.m_vecMessage[i+1] );
					continue;
				}
				pField->markDirty();
			}
			m_bERVReady = true;
			break;
		}

		default:
			break;
	}
}

void BroanComponent::handleRemotePassThroughFrame(const BroanFrame &frame)
{
	if( m_bPrivateControlSession )
	{
		ESP_LOGD("broan", "Dropping remote frame while ESP has private control");
		return;
	}

	writeRawToHrv( frame.m_vecRaw );

	if( frame.m_nTarget == m_nServerAddress && !frame.m_vecMessage.empty() && frame.m_vecMessage[0] == 0x03 )
		m_bWaitForRemote = false;
}

void BroanComponent::processUartDiagnostic()
{
	if( m_bUartDiagnosticFinished )
		return;

	uint32_t now = millis();
	if( now - m_unUartDiagnosticBootAt < UART_DIAGNOSTIC_START_DELAY_MS )
		return;

	if( m_bUartDiagnosticPassThroughMode )
	{
		processUartDiagnosticPassThrough();
		return;
	}

	if( !m_bUartDiagnosticAttemptActive )
		startUartDiagnosticAttempt();

	processDiagnosticUart( this->parent_, m_HrvDiagnosticStats, "HRV", DiagnosticSideHrv, false );
	processDiagnosticUart( this->remote_uart_, m_RemoteDiagnosticStats, "remote", DiagnosticSideRemote, false );

	if( hasDiagnosticSuccess() )
	{
		enterUartDiagnosticPassThroughMode();
		processUartDiagnosticPassThrough();
		return;
	}

	if( now - m_unUartDiagnosticAttemptStartedAt >= UART_DIAGNOSTIC_ATTEMPT_MS )
		finishUartDiagnosticAttempt( false );
}

void BroanComponent::enterUartDiagnosticPassThroughMode()
{
	if( m_bUartDiagnosticPassThroughMode )
		return;

	finalizeDiagnosticBurst( m_HrvDiagnosticStats );
	finalizeDiagnosticBurst( m_RemoteDiagnosticStats );
	logDiagnosticReport( true );

	BroanDiagnosticSide side = DiagnosticSideNone;
	firstDiagnosticFrame( &side );
	uint32_t baud = UART_DIAGNOSTIC_BAUD_RATES[m_unUartDiagnosticBaudIndex];

	m_bUartDiagnosticPassThroughMode = true;
	m_bUartDiagnosticAttemptActive = false;
	m_UartDiagnosticFirstValidSide = side;
	m_unUartDiagnosticPassThroughStartedAt = millis();
	m_unUartDiagnosticLastPassThroughReportAt = m_unUartDiagnosticPassThroughStartedAt;
	m_unUartDiagnosticHrvForwardedFrames = 0;
	m_unUartDiagnosticRemoteForwardedFrames = 0;

	ESP_LOGW("broan", "################ BROAN UART DIAGNOSTIC FIRST VALID SIDE ################");
	ESP_LOGW("broan", "Pinned UART config: baud_rate=%u, inverted=%s", static_cast<unsigned>( baud ), m_bUartDiagnosticInverted ? "true" : "false" );
	if( m_bUartDiagnosticCandidateClientAddressValid )
		ESP_LOGW("broan", "Confirmed client address from remote side: 0x%02X", m_nUartDiagnosticCandidateClientAddress );
	else
		ESP_LOGW("broan", "Client address is not confirmed yet; HRV Ping targets are probe addresses until a remote response is seen.");
	if( m_unUartDiagnosticHrvProbeTargetCount > 0 )
	{
		std::string targets = formatDiagnosticHrvProbeTargets();
		ESP_LOGW("broan", "Observed HRV Ping probe targets: %s", targets.c_str() );
	}
	ESP_LOGW("broan", "First valid side: %s", diagnosticSideLabel( side ) );
	ESP_LOGW("broan", "Continuing in diagnostic pass-through mode until both UART sides carry valid frames and the remote side confirms client_address.");
	if( !remote_uart_ )
		ESP_LOGW("broan", "Remote UART is not configured, so complete pass-through validation cannot finish.");

	forwardDiagnosticStoredFrames( DiagnosticSideHrv );
	forwardDiagnosticStoredFrames( DiagnosticSideRemote );
}

void BroanComponent::processUartDiagnosticPassThrough()
{
	processDiagnosticUart( this->parent_, m_HrvDiagnosticStats, "HRV", DiagnosticSideHrv, true );
	processDiagnosticUart( this->remote_uart_, m_RemoteDiagnosticStats, "remote", DiagnosticSideRemote, true );

	if( hasDiagnosticPassThroughSuccess() )
	{
		finishUartDiagnosticPassThroughSuccess();
		return;
	}

	uint32_t now = millis();
	if( now - m_unUartDiagnosticLastPassThroughReportAt >= UART_DIAGNOSTIC_PASS_THROUGH_REPORT_MS )
	{
		m_unUartDiagnosticLastPassThroughReportAt = now;
		logDiagnosticPassThroughStatus();
	}
}

void BroanComponent::startUartDiagnosticAttempt()
{
	uint32_t baud = UART_DIAGNOSTIC_BAUD_RATES[m_unUartDiagnosticBaudIndex];
	m_unUartDiagnosticAttemptCount++;
	m_unUartDiagnosticAttemptStartedAt = millis();
	m_bUartDiagnosticAttemptActive = true;
	m_bUartDiagnosticCandidateClientAddressValid = false;
	m_nUartDiagnosticCandidateClientAddress = 0;
	m_UartDiagnosticFirstValidSide = DiagnosticSideNone;
	m_unUartDiagnosticHrvForwardedFrames = 0;
	m_unUartDiagnosticRemoteForwardedFrames = 0;
	m_unUartDiagnosticHrvProbeTargetCount = 0;
	for( bool &seen : m_rgUartDiagnosticHrvProbeTargets )
		seen = false;

	resetDiagnosticStats( m_HrvDiagnosticStats );
	resetDiagnosticStats( m_RemoteDiagnosticStats );

	ESP_LOGW("broan", "================ BROAN UART DIAGNOSTIC ATTEMPT %u ================", static_cast<unsigned>( m_unUartDiagnosticAttemptCount ) );
	ESP_LOGW("broan", "Trying baud_rate=%u, inverted=%s for %ums", static_cast<unsigned>( baud ), m_bUartDiagnosticInverted ? "true" : "false", UART_DIAGNOSTIC_ATTEMPT_MS );
	ESP_LOGW("broan", "Both configured UARTs are tested independently; one valid side is enough to pin the UART config.");

	configureDiagnosticUart( this->parent_, baud, m_bUartDiagnosticInverted, "HRV" );
	configureDiagnosticUart( this->remote_uart_, baud, m_bUartDiagnosticInverted, "remote" );
}

void BroanComponent::finishUartDiagnosticAttempt(bool success)
{
	if( !m_bUartDiagnosticAttemptActive )
		return;

	finalizeDiagnosticBurst( m_HrvDiagnosticStats );
	finalizeDiagnosticBurst( m_RemoteDiagnosticStats );
	logDiagnosticReport( success );

	m_bUartDiagnosticAttemptActive = false;

	if( success )
	{
		const char *label = nullptr;
		const BroanFrame *frame = firstDiagnosticFrame( &label );
		uint32_t baud = UART_DIAGNOSTIC_BAUD_RATES[m_unUartDiagnosticBaudIndex];

		ESP_LOGW("broan", "################ BROAN UART DIAGNOSTIC SUCCESS ################");
		ESP_LOGW("broan", "Use baud_rate=%u and tx_pin/rx_pin inverted=%s", static_cast<unsigned>( baud ), m_bUartDiagnosticInverted ? "true" : "false" );

		if( frame )
		{
			uint8_t type = frame->m_vecMessage.empty() ? 0xFF : frame->m_vecMessage[0];
			std::string raw = formatBytes( frame->m_vecRaw );
			std::string message = formatBytes( frame->m_vecMessage );
			ESP_LOGW("broan", "Valid side=%s, target=0x%02X, sender=0x%02X, message_type=0x%02X, address_hint=0x%02X",
				label ? label : "unknown",
				frame->m_nTarget,
				frame->m_nSender,
				type,
				inferClientAddress( *frame ) );
			ESP_LOGW("broan", "Valid raw frame: %s", raw.c_str() );
			ESP_LOGW("broan", "Valid message: %s", message.c_str() );
		}

		ESP_LOGW("broan", "UART diagnostic mode is now stopped. Disable uart_diagnostic after copying the working config.");
		m_bUartDiagnosticFinished = true;
		return;
	}

	advanceUartDiagnosticAttempt();
}

void BroanComponent::finishUartDiagnosticPassThroughSuccess()
{
	if( m_bUartDiagnosticFinished )
		return;

	finalizeDiagnosticBurst( m_HrvDiagnosticStats );
	finalizeDiagnosticBurst( m_RemoteDiagnosticStats );
	logDiagnosticReport( true );

	uint32_t baud = UART_DIAGNOSTIC_BAUD_RATES[m_unUartDiagnosticBaudIndex];

	ESP_LOGW("broan", "################ BROAN UART PASS-THROUGH DIAGNOSTIC SUCCESS ################");
	ESP_LOGW("broan", "Both UART sides received valid Broan frames and frames were forwarded in both directions.");
	ESP_LOGW("broan", "Use baud_rate=%u and tx_pin/rx_pin inverted=%s", static_cast<unsigned>( baud ), m_bUartDiagnosticInverted ? "true" : "false" );
	if( m_bUartDiagnosticCandidateClientAddressValid )
		ESP_LOGW("broan", "Use client_address: 0x%02X", m_nUartDiagnosticCandidateClientAddress );
	else
		ESP_LOGW("broan", "Client address was not confirmed from the remote side.");
	ESP_LOGW("broan", "Forwarded HRV->remote frames=%u, remote->HRV frames=%u",
		static_cast<unsigned>( m_unUartDiagnosticHrvForwardedFrames ),
		static_cast<unsigned>( m_unUartDiagnosticRemoteForwardedFrames ) );
	ESP_LOGW("broan", "UART diagnostic mode is now stopped. Disable uart_diagnostic after copying the working config.");

	m_bUartDiagnosticFinished = true;
}

void BroanComponent::advanceUartDiagnosticAttempt()
{
	if( !m_bUartDiagnosticInverted )
	{
		m_bUartDiagnosticInverted = true;
		return;
	}

	m_bUartDiagnosticInverted = false;
	m_unUartDiagnosticBaudIndex++;
	if( m_unUartDiagnosticBaudIndex < sizeof(UART_DIAGNOSTIC_BAUD_RATES) / sizeof(UART_DIAGNOSTIC_BAUD_RATES[0]) )
		return;

	m_unUartDiagnosticBaudIndex = 0;
	ESP_LOGW("broan", "################################################################");
	ESP_LOGW("broan", "### BROAN UART DIAGNOSTIC: ALL BAUD/POLARITY ATTEMPTS FAILED ###");
	ESP_LOGW("broan", "### CYCLING BACK TO 9600 BAUD NOW                            ###");
	ESP_LOGW("broan", "################################################################");
}

void BroanComponent::configureDiagnosticUart(uart::UARTComponent *uart, uint32_t baud, bool inverted, const char *label)
{
	if( !uart )
	{
		ESP_LOGW("broan", "Diagnostic %s UART: not configured", label );
		return;
	}

	uart->set_baud_rate( baud );
#if defined(USE_ESP32) || defined(USE_ESP8266)
	uart->load_settings( false );
#endif

#ifdef USE_ESP32
	auto *idf_uart = static_cast<uart::IDFUARTComponent *>( uart );
	uint32_t invert_mask = inverted ? ( UART_SIGNAL_TXD_INV | UART_SIGNAL_RXD_INV ) : 0;
	esp_err_t err = uart_set_line_inverse( static_cast<uart_port_t>( idf_uart->get_hw_serial_number() ), invert_mask );
	if( err != ESP_OK )
		ESP_LOGW("broan", "Diagnostic %s UART: uart_set_line_inverse failed: %s", label, esp_err_to_name(err) );
#else
	if( inverted )
		ESP_LOGW("broan", "Diagnostic %s UART: runtime inversion is only implemented for ESP32", label );
#endif

	drainDiagnosticUart( uart );
	ESP_LOGW("broan", "Diagnostic %s UART configured: baud_rate=%u, inverted=%s", label, static_cast<unsigned>( baud ), inverted ? "true" : "false" );
}

void BroanComponent::drainDiagnosticUart(uart::UARTComponent *uart)
{
	if( !uart )
		return;

	uint8_t discard;
	size_t drained = 0;
	while( uart->available() > 0 && drained < 1024 )
	{
		if( !uart->read_byte( &discard ) )
			break;
		drained++;
	}
}

void BroanComponent::resetDiagnosticStats(BroanDiagnosticSideStats &stats)
{
	stats.m_bReceivedData = false;
	stats.m_unByteCount = 0;
	stats.m_unInvalidStartBytes = 0;
	stats.m_unInvalidFrameCount = 0;
	stats.m_unDroppedBurstCount = 0;
	stats.m_vecBursts.clear();
	stats.m_CurrentBurst = BroanDiagnosticBurst{};
	stats.m_unLastByteAt = 0;
	stats.m_vecFrameBuffer.clear();
	stats.m_unValidFrameCount = 0;
	stats.m_vecValidFrames.clear();
}

void BroanComponent::finalizeDiagnosticBurst(BroanDiagnosticSideStats &stats)
{
	if( stats.m_CurrentBurst.m_vecBytes.empty() )
		return;

	if( stats.m_vecBursts.size() < UART_DIAGNOSTIC_MAX_BURSTS )
		stats.m_vecBursts.push_back( stats.m_CurrentBurst );
	else
		stats.m_unDroppedBurstCount++;

	stats.m_CurrentBurst = BroanDiagnosticBurst{};
}

void BroanComponent::processDiagnosticUart(uart::UARTComponent *uart, BroanDiagnosticSideStats &stats, const char *label, BroanDiagnosticSide side, bool forward_valid_frames)
{
	if( !uart )
		return;

	uint32_t now = millis();
	if( !stats.m_CurrentBurst.m_vecBytes.empty() && now - stats.m_unLastByteAt >= UART_DIAGNOSTIC_BURST_IDLE_MS )
		finalizeDiagnosticBurst( stats );

	size_t processed = 0;
	while( uart->available() > 0 && processed < UART_DIAGNOSTIC_MAX_BYTES_PER_LOOP )
	{
		uint8_t value;
		if( !uart->read_byte( &value ) )
			break;

		BroanFrame frame;
		if( processDiagnosticByte( stats, value, frame ) )
		{
			stats.m_unValidFrameCount++;
			if( stats.m_vecValidFrames.size() < UART_DIAGNOSTIC_MAX_VALID_FRAMES )
				stats.m_vecValidFrames.push_back( frame );
			ESP_LOGW("broan", "Diagnostic %s UART: valid Broan frame found", label );
			updateDiagnosticAddressLearning( side, frame );
			if( forward_valid_frames )
				forwardDiagnosticFrame( side, frame );
		}

		processed++;
	}
}

bool BroanComponent::processDiagnosticByte(BroanDiagnosticSideStats &stats, uint8_t value, BroanFrame &frame)
{
	uint32_t now = millis();
	if( !stats.m_CurrentBurst.m_vecBytes.empty() && now - stats.m_unLastByteAt >= UART_DIAGNOSTIC_BURST_IDLE_MS )
		finalizeDiagnosticBurst( stats );

	stats.m_bReceivedData = true;
	stats.m_unByteCount++;
	stats.m_unLastByteAt = now;

	if( stats.m_CurrentBurst.m_vecBytes.size() < UART_DIAGNOSTIC_MAX_BURST_BYTES )
	{
		stats.m_CurrentBurst.m_vecBytes.push_back( value );
	}
	else
	{
		stats.m_CurrentBurst.m_bTruncated = true;
		stats.m_CurrentBurst.m_unOmittedBytes++;
	}

	if( stats.m_vecFrameBuffer.empty() && value != 0x01 )
	{
		stats.m_unInvalidStartBytes++;
		return false;
	}

	stats.m_vecFrameBuffer.push_back( value );
	size_t pos = stats.m_vecFrameBuffer.size() - 1;

	if( ( pos == 1 || pos == 2 ) && value > 32 )
	{
		stats.m_unInvalidFrameCount++;
		stats.m_vecFrameBuffer.clear();
		return false;
	}

	if( pos == 3 && value != 0x01 )
	{
		stats.m_unInvalidFrameCount++;
		stats.m_vecFrameBuffer.clear();
		return false;
	}

	if( stats.m_vecFrameBuffer.size() < 5 )
		return false;

	size_t len = stats.m_vecFrameBuffer[4];
	size_t total = 5 + len + 2;
	if( stats.m_vecFrameBuffer.size() < total )
		return false;

	if( stats.m_vecFrameBuffer[total - 1] != 0x04 )
	{
		stats.m_unInvalidFrameCount++;
		stats.m_vecFrameBuffer.clear();
		return false;
	}

	std::vector<uint8_t> message(
		stats.m_vecFrameBuffer.begin() + 5,
		stats.m_vecFrameBuffer.begin() + 5 + len
	);

	uint8_t checksum = stats.m_vecFrameBuffer[5 + len];
	uint8_t expected_checksum = calculateChecksum( stats.m_vecFrameBuffer[2], stats.m_vecFrameBuffer[1], message );
	if( checksum != expected_checksum )
	{
		stats.m_unInvalidFrameCount++;
		stats.m_vecFrameBuffer.clear();
		return false;
	}

	frame.m_nTarget = stats.m_vecFrameBuffer[1];
	frame.m_nSender = stats.m_vecFrameBuffer[2];
	frame.m_vecMessage = message;
	frame.m_vecRaw = stats.m_vecFrameBuffer;
	stats.m_vecFrameBuffer.clear();

	return true;
}

bool BroanComponent::hasDiagnosticSuccess() const
{
	return m_HrvDiagnosticStats.m_unValidFrameCount > 0 || m_RemoteDiagnosticStats.m_unValidFrameCount > 0;
}

bool BroanComponent::hasDiagnosticPassThroughSuccess() const
{
	return m_HrvDiagnosticStats.m_unValidFrameCount > 0
		&& m_RemoteDiagnosticStats.m_unValidFrameCount > 0
		&& m_unUartDiagnosticHrvForwardedFrames > 0
		&& m_unUartDiagnosticRemoteForwardedFrames > 0
		&& m_bUartDiagnosticCandidateClientAddressValid;
}

void BroanComponent::updateDiagnosticAddressLearning(BroanDiagnosticSide side, const BroanFrame &frame)
{
	if( side == DiagnosticSideHrv && isDiagnosticHrvProbeFrame( frame ) )
	{
		recordDiagnosticHrvProbeTarget( frame.m_nTarget );
		return;
	}

	if( side != DiagnosticSideRemote )
		return;

	if( frame.m_nSender == m_nServerAddress )
		return;

	uint8_t address = frame.m_nSender;

	if( !m_bUartDiagnosticCandidateClientAddressValid || m_nUartDiagnosticCandidateClientAddress != address )
	{
		m_nUartDiagnosticCandidateClientAddress = address;
		m_bUartDiagnosticCandidateClientAddressValid = true;
		ESP_LOGW("broan", "Diagnostic remote UART: confirmed client_address=0x%02X from frame target=0x%02X sender=0x%02X type=0x%02X",
			m_nUartDiagnosticCandidateClientAddress,
			frame.m_nTarget,
			frame.m_nSender,
			frame.m_vecMessage.empty() ? 0xFF : frame.m_vecMessage[0] );
	}
}

void BroanComponent::recordDiagnosticHrvProbeTarget(uint8_t target)
{
	if( target >= sizeof( m_rgUartDiagnosticHrvProbeTargets ) / sizeof( m_rgUartDiagnosticHrvProbeTargets[0] ) )
		return;

	if( m_rgUartDiagnosticHrvProbeTargets[target] )
		return;

	m_rgUartDiagnosticHrvProbeTargets[target] = true;
	m_unUartDiagnosticHrvProbeTargetCount++;
	ESP_LOGW("broan", "Diagnostic HRV UART: observed HRV Ping probe target=0x%02X (%u unique targets)",
		target,
		static_cast<unsigned>( m_unUartDiagnosticHrvProbeTargetCount ) );
}

bool BroanComponent::isDiagnosticHrvProbeFrame(const BroanFrame &frame) const
{
	return frame.m_nSender == m_nServerAddress
		&& !frame.m_vecMessage.empty()
		&& frame.m_vecMessage[0] == 0x02;
}

void BroanComponent::forwardDiagnosticStoredFrames(BroanDiagnosticSide side)
{
	const BroanDiagnosticSideStats *stats = nullptr;
	if( side == DiagnosticSideHrv )
		stats = &m_HrvDiagnosticStats;
	else if( side == DiagnosticSideRemote )
		stats = &m_RemoteDiagnosticStats;

	if( !stats )
		return;

	for( const BroanFrame &frame : stats->m_vecValidFrames )
		forwardDiagnosticFrame( side, frame );
}

void BroanComponent::forwardDiagnosticFrame(BroanDiagnosticSide side, const BroanFrame &frame)
{
	if( frame.m_vecRaw.empty() )
		return;

	if( side == DiagnosticSideHrv )
	{
		writeRawToRemote( frame.m_vecRaw );
		m_unUartDiagnosticHrvForwardedFrames++;
		if( isDiagnosticHrvProbeFrame( frame ) )
		{
			ESP_LOGD("broan", "Diagnostic pass-through forwarded HRV Ping probe to remote target=0x%02X sender=0x%02X",
				frame.m_nTarget,
				frame.m_nSender );
		}
		else
		{
			ESP_LOGW("broan", "Diagnostic pass-through forwarded HRV->remote frame type=0x%02X target=0x%02X sender=0x%02X",
				frame.m_vecMessage.empty() ? 0xFF : frame.m_vecMessage[0],
				frame.m_nTarget,
				frame.m_nSender );
		}
		return;
	}

	if( side == DiagnosticSideRemote )
	{
		if( frame.m_nSender == m_nServerAddress )
		{
			ESP_LOGD("broan", "Diagnostic pass-through ignored remote-side server-origin frame target=0x%02X sender=0x%02X",
				frame.m_nTarget,
				frame.m_nSender );
			return;
		}

		writeRawToHrv( frame.m_vecRaw );
		m_unUartDiagnosticRemoteForwardedFrames++;
		ESP_LOGW("broan", "Diagnostic pass-through forwarded remote->HRV frame type=0x%02X target=0x%02X sender=0x%02X",
			frame.m_vecMessage.empty() ? 0xFF : frame.m_vecMessage[0],
			frame.m_nTarget,
			frame.m_nSender );
	}
}

void BroanComponent::logDiagnosticPassThroughStatus()
{
	ESP_LOGW("broan", "Broan UART diagnostic pass-through still waiting: first_valid_side=%s HRV_valid=%u remote_valid=%u HRV->remote=%u remote->HRV=%u elapsed=%ums",
		diagnosticSideLabel( m_UartDiagnosticFirstValidSide ),
		static_cast<unsigned>( m_HrvDiagnosticStats.m_unValidFrameCount ),
		static_cast<unsigned>( m_RemoteDiagnosticStats.m_unValidFrameCount ),
		static_cast<unsigned>( m_unUartDiagnosticHrvForwardedFrames ),
		static_cast<unsigned>( m_unUartDiagnosticRemoteForwardedFrames ),
		static_cast<unsigned>( millis() - m_unUartDiagnosticPassThroughStartedAt ) );

	if( m_bUartDiagnosticCandidateClientAddressValid )
	{
		ESP_LOGW("broan", "Broan UART diagnostic confirmed client_address=0x%02X", m_nUartDiagnosticCandidateClientAddress );
	}
	else
	{
		ESP_LOGW("broan", "Broan UART diagnostic is waiting for a remote-side response to confirm client_address.");
		if( m_unUartDiagnosticHrvProbeTargetCount > 0 )
		{
			std::string targets = formatDiagnosticHrvProbeTargets();
			ESP_LOGW("broan", "Observed HRV Ping probe targets so far: %s", targets.c_str() );
		}
	}
}

const char* BroanComponent::diagnosticSideLabel(BroanDiagnosticSide side) const
{
	switch( side )
	{
		case DiagnosticSideHrv:
			return "HRV";
		case DiagnosticSideRemote:
			return "remote";
		default:
			return "none";
	}
}

const BroanFrame* BroanComponent::firstDiagnosticFrame(const BroanDiagnosticSideStats &stats) const
{
	if( stats.m_vecValidFrames.empty() )
		return nullptr;
	return &stats.m_vecValidFrames.front();
}

const BroanFrame* BroanComponent::firstDiagnosticFrame(const char **label) const
{
	const BroanFrame *frame = firstDiagnosticFrame( m_HrvDiagnosticStats );
	if( frame )
	{
		if( label )
			*label = "HRV";
		return frame;
	}

	frame = firstDiagnosticFrame( m_RemoteDiagnosticStats );
	if( frame && label )
		*label = "remote";

	return frame;
}

const BroanFrame* BroanComponent::firstDiagnosticFrame(BroanDiagnosticSide *side) const
{
	const BroanFrame *frame = firstDiagnosticFrame( m_HrvDiagnosticStats );
	if( frame )
	{
		if( side )
			*side = DiagnosticSideHrv;
		return frame;
	}

	frame = firstDiagnosticFrame( m_RemoteDiagnosticStats );
	if( frame && side )
		*side = DiagnosticSideRemote;

	if( !frame && side )
		*side = DiagnosticSideNone;

	return frame;
}

uint8_t BroanComponent::inferClientAddress(const BroanFrame &frame) const
{
	if( frame.m_nSender == m_nServerAddress )
		return frame.m_nTarget;
	if( frame.m_nTarget == m_nServerAddress )
		return frame.m_nSender;
	return frame.m_nTarget;
}

std::string BroanComponent::formatDiagnosticHrvProbeTargets() const
{
	if( m_unUartDiagnosticHrvProbeTargetCount == 0 )
		return "(none)";

	std::string out;
	for( size_t i=0; i<sizeof( m_rgUartDiagnosticHrvProbeTargets ) / sizeof( m_rgUartDiagnosticHrvProbeTargets[0] ); i++ )
	{
		if( !m_rgUartDiagnosticHrvProbeTargets[i] )
			continue;

		char buffer[6];
		snprintf( buffer, sizeof(buffer), "0x%02X", static_cast<unsigned>( i ) );
		if( !out.empty() )
			out += ' ';
		out += buffer;
	}

	return out.empty() ? "(none)" : out;
}

void BroanComponent::logDiagnosticReport(bool success)
{
	uint32_t baud = UART_DIAGNOSTIC_BAUD_RATES[m_unUartDiagnosticBaudIndex];
	ESP_LOGW("broan", "---------------- BROAN UART DIAGNOSTIC REPORT ----------------");
	ESP_LOGW("broan", "Attempt %u result=%s baud_rate=%u inverted=%s elapsed=%ums",
		static_cast<unsigned>( m_unUartDiagnosticAttemptCount ),
		success ? "VALID FRAME FOUND" : "no valid frame",
		static_cast<unsigned>( baud ),
		m_bUartDiagnosticInverted ? "true" : "false",
		static_cast<unsigned>( millis() - m_unUartDiagnosticAttemptStartedAt ) );

	logDiagnosticSideReport( "HRV", m_HrvDiagnosticStats );
	if( remote_uart_ )
		logDiagnosticSideReport( "remote", m_RemoteDiagnosticStats );
	else
		ESP_LOGW("broan", "remote side: UART not configured");

	ESP_LOGW("broan", "----------------------------------------------------------------");
}

void BroanComponent::logDiagnosticSideReport(const char *label, const BroanDiagnosticSideStats &stats)
{
	ESP_LOGW("broan", "%s side: data_received=%s bytes=%u valid_frames=%u invalid_start_bytes=%u invalid_frames=%u bursts=%u dropped_bursts=%u",
		label,
		stats.m_bReceivedData ? "yes" : "no",
		static_cast<unsigned>( stats.m_unByteCount ),
		static_cast<unsigned>( stats.m_unValidFrameCount ),
		static_cast<unsigned>( stats.m_unInvalidStartBytes ),
		static_cast<unsigned>( stats.m_unInvalidFrameCount ),
		static_cast<unsigned>( stats.m_vecBursts.size() ),
		static_cast<unsigned>( stats.m_unDroppedBurstCount ) );

	for( size_t i=0; i<stats.m_vecBursts.size(); i++ )
	{
		const BroanDiagnosticBurst &burst = stats.m_vecBursts[i];
		std::string hex = formatBytes( burst.m_vecBytes );
		if( burst.m_bTruncated )
		{
			ESP_LOGW("broan", "%s burst %u: bytes=%u truncated omitted=%u bytes",
				label,
				static_cast<unsigned>( i + 1 ),
				static_cast<unsigned>( burst.m_vecBytes.size() ),
				static_cast<unsigned>( burst.m_unOmittedBytes ) );
		}
		else
		{
			ESP_LOGW("broan", "%s burst %u: bytes=%u",
				label,
				static_cast<unsigned>( i + 1 ),
				static_cast<unsigned>( burst.m_vecBytes.size() ) );
		}
		ESP_LOGW("broan", "%s burst %u hex: %s", label, static_cast<unsigned>( i + 1 ), hex.c_str() );
	}

	if( !stats.m_vecFrameBuffer.empty() )
	{
		std::string partial = formatBytes( stats.m_vecFrameBuffer );
		ESP_LOGW("broan", "%s partial frame candidate: %s", label, partial.c_str() );
	}

	for( size_t i=0; i<stats.m_vecValidFrames.size(); i++ )
	{
		const BroanFrame &frame = stats.m_vecValidFrames[i];
		uint8_t type = frame.m_vecMessage.empty() ? 0xFF : frame.m_vecMessage[0];
		std::string raw = formatBytes( frame.m_vecRaw );
		std::string message = formatBytes( frame.m_vecMessage );
		ESP_LOGW("broan", "%s valid frame %u: target=0x%02X sender=0x%02X type=0x%02X address_hint=0x%02X",
			label,
			static_cast<unsigned>( i + 1 ),
			frame.m_nTarget,
			frame.m_nSender,
			type,
			inferClientAddress( frame ) );
		ESP_LOGW("broan", "%s valid frame %u raw: %s", label, static_cast<unsigned>( i + 1 ), raw.c_str() );
		ESP_LOGW("broan", "%s valid frame %u message: %s", label, static_cast<unsigned>( i + 1 ), message.c_str() );
	}

	if( stats.m_unValidFrameCount > stats.m_vecValidFrames.size() )
	{
		ESP_LOGW("broan", "%s valid frames stored first %u of %u",
			label,
			static_cast<unsigned>( stats.m_vecValidFrames.size() ),
			static_cast<unsigned>( stats.m_unValidFrameCount ) );
	}
}

std::string BroanComponent::formatBytes(const std::vector<uint8_t> &bytes) const
{
	if( bytes.empty() )
		return "(empty)";

	std::string out;
	out.reserve( bytes.size() * 3 );
	for( size_t i=0; i<bytes.size(); i++ )
	{
		char buffer[4];
		snprintf( buffer, sizeof(buffer), "%02X", bytes[i] );
		if( i > 0 )
			out += ' ';
		out += buffer;
	}
	return out;
}

void esp_log_vector_hex(const char* tag, const std::vector<uint8_t>& message) {
    if (message.empty()) {
        ESP_LOGW(tag, "Message vector is empty");
        return;
    }
    std::string hex_string;
    for (size_t i = 0; i < message.size(); ++i) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X ", message[i]);
        hex_string += buf;
        // Optional: newline every 16 bytes for readability
        if ((i + 1) % 16 == 0) {
            ESP_LOGW(tag, "%s", hex_string.c_str());
            hex_string.clear();
        }
    }
    if (!hex_string.empty()) {
        ESP_LOGW(tag, "%s", hex_string.c_str());
    }
}

void BroanComponent::handleMessage(uint8_t sender, uint8_t target, const std::vector<uint8_t>& message)
{
	if( target == m_nServerAddress )
	{
		if( message[0] == 0x03 )
			m_bWaitForRemote = false;
	}
#ifndef LISTEN_ONLY
	if (target != m_nClientAddress) return;
#endif

	int m_nType = message[0];
	switch (m_nType)
	{
		case 0x02:
		{
			// Respond to ping
			std::vector<uint8_t> reply = {0x03};
			reply.insert(reply.end(), message.begin() + 1, message.end());

			send(reply);

			ESP_LOGD("broan","0x02 Ping");
			m_bERVReady = true;
			break;
		}
		case 0x04:
		{
			// Flow control
			m_nLastHadControl = millis();
			m_bHaveControl = true;
			m_bExpectingReply = false;
			// ERV won't re-ping us if we drop, so just assume if we're getting flow
			// control messages it's ready for us to start feeding it data.
			m_bERVReady = true;

			// Ack that we have control. We'll send any queued messages then release with 0x04
			send({ 0x05 });
			//ESP_LOGD("broan","Got flow control");
			break;
		}
		case 0x05:
			// ERV has confirmed it has control, no-op
			break;

		case 0x41:
		{
			// set register ACK, mark all fields dirty
			for( int i=1; i<message.size(); i+=2)
			{
				BroanField_t *pField = lookupField(message[i], message[i+1]);
				if( !pField )
				{
					ESP_LOGW("broan", "Got write response for unknown field %02X %02X", message[i], message[i+1]);
					continue;
				}
				pField->markDirty();
			}
			m_bExpectingReply = false;


			break;
		}
		case 0x21:
		{
			// Request register response
			parseBroanFields(message);
			m_bExpectingReply = false;

			break;
		}
#ifdef LISTEN_ONLY
		case 0x20:
			break;
#endif
		default:
		{
			// Log unhandled m_nType
			ESP_LOGW("broan", "Unhandled m_nType %02X", m_nType);
			esp_log_vector_hex("broan", message );
			break;
		}
	}
}

void BroanComponent::replyIfAllowed()
{
	uint32_t time = millis();
	if( m_nLastHadControl + CONTROL_TIMEOUT < time )
	{
		ESP_LOGW("broan","ERV has not yielded control in over %ims, communication has likely failed. Please restart the device.", CONTROL_TIMEOUT);
		m_bERVReady = false;
		m_nLastHadControl = time;
		if( isPassThroughEnabled() )
		{
			m_bHaveControl = false;
			m_bExpectingReply = false;
			m_bHaveSentMessage = false;
			m_bPrivateControlSession = false;
		}
	}

	if( !m_bHaveControl || m_bExpectingReply )
		return;

	if( m_vecSendQueue.size() > 0 )
	{
		send( m_vecSendQueue.front() );
		m_vecSendQueue.pop_front();
		m_bExpectingReply = true;
		m_bHaveSentMessage = true;
		return;
	}

	if( m_bHaveControl && !m_bExpectingReply && m_vecSendQueue.size() == 0 )
	{
		// Release control.
		send( { 0x04 } );
		m_bHaveControl = false;
		m_bERVReady = true;
		m_bHaveSentMessage = false;
		m_bPrivateControlSession = false;
		return;
	}

}

void BroanComponent::queueMessage(std::vector<uint8_t>& message)
{
	if( m_vecSendQueue.size() > 20 )
	{
		ESP_LOGW("broan","Dropping queued message: Stack is full. (Tried to queue %02X)",message[0]);
		return;
	}
	m_vecSendQueue.push_back(message);
}

std::string BroanComponent::fanModeToString(uint8_t value) const
{
	switch( value )
	{
		case BroanFanMode::Ovr:
			return "ovr";
		case BroanFanMode::Intermittent:
			return "int";
		case BroanFanMode::Min:
			return "min";
		case BroanFanMode::Max:
			return "max";
		case BroanFanMode::Medium:
			return "medium";
		case BroanFanMode::Turbo:
			return "turbo";
		case BroanFanMode::Humidity:
			return "humidity";
		case BroanFanMode::Recirculate:
			return "recirculate";
		default:
			return "off";
	}
}

void BroanComponent::publishFanModeState(uint8_t value)
{
#ifdef USE_SELECT
	if( !fan_mode_select_ )
		return;

	fan_mode_select_->publish_state( fanModeToString( value ) );
#endif
}

void BroanComponent::startFanModeOptimistic(uint8_t value)
{
	m_bFanModeOptimistic = true;
	m_nFanModeOptimisticValue = value;
	m_unFanModeOptimisticUntil = millis() + FAN_MODE_OPTIMISTIC_HOLD_MS;
	publishFanModeState( value );
	ESP_LOGD("broan", "Optimistically publishing fan mode %s while waiting for HRV acknowledgement",
		fanModeToString( value ).c_str() );
}

bool BroanComponent::shouldSuppressFanModePublish(uint8_t reported_value, bool *force_publish)
{
	if( force_publish )
		*force_publish = false;

	if( !m_bFanModeOptimistic )
		return false;

	if( reported_value == m_nFanModeOptimisticValue )
	{
		m_bFanModeOptimistic = false;
		ESP_LOGD("broan", "HRV confirmed fan mode %s", fanModeToString( reported_value ).c_str() );
		return false;
	}

	uint32_t now = millis();
	if( now < m_unFanModeOptimisticUntil )
	{
		ESP_LOGD("broan", "Suppressing stale fan mode %s while waiting for %s",
			fanModeToString( reported_value ).c_str(),
			fanModeToString( m_nFanModeOptimisticValue ).c_str() );
		return true;
	}

	m_bFanModeOptimistic = false;
	if( force_publish )
		*force_publish = true;
	ESP_LOGW("broan", "Fan mode %s was not confirmed within %ums; publishing reported mode %s",
		fanModeToString( m_nFanModeOptimisticValue ).c_str(),
		static_cast<unsigned>( FAN_MODE_OPTIMISTIC_HOLD_MS ),
		fanModeToString( reported_value ).c_str() );
	return false;
}


void BroanComponent::parseBroanFields(const std::vector<uint8_t>& message)
{
    size_t i = 1;
	bool bPublish = false;

    while (i < message.size())
    {
        uint8_t nOpcodeHigh = message[i++];
        uint8_t nOpcodeLow  = message[i++];
		size_t len = message[i++];
		uint32_t nDataPos = i;

		i += len;

		uint32_t unField = lookupFieldIndex(nOpcodeHigh, nOpcodeLow);
		if( unField == INVALID_FIELD )
			continue;

		BroanField_t *pField = &m_vecFields[unField];
		if( !pField )
		{
			handleUnknownField(nOpcodeHigh, nOpcodeLow, len, nDataPos, message);
			continue;
		}

		uint32_t oldVal = pField->m_value.m_nValue;
		for (size_t b = 0; b < len; ++b)
			pField->m_value.m_rgBytes[b] = static_cast<char>(message[nDataPos+b]);

		pField->m_unLastUpdate = millis();

		bool bForcePublish = false;
		if( unField == BroanField::FanMode && shouldSuppressFanModePublish( pField->m_value.m_chValue, &bForcePublish ) )
			continue;
	
		if( oldVal == pField->m_value.m_nValue && !bForcePublish )
			continue;

		switch(unField)
		{
#ifdef USE_SELECT
			case BroanField::FanMode:
			{
				publishFanModeState( pField->m_value.m_chValue );
			}
			break;
#endif
#ifdef USE_SENSOR
			case BroanField::Wattage:
				if( !power_sensor_ )
					continue;

				power_sensor_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::FilterLife:
				if( !filter_life_sensor_ )
					continue;

				// Seconds -> Days
				filter_life_sensor_->publish_state(pField->m_value.m_nValue / ( 60 * 60 * 24 ) );
			break;

			case BroanField::TemperatureIn:
				if( !temperature_sensor_ )
					continue;

				temperature_sensor_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::SupplyCFM:
				if( !supply_cfm_sensor_ )
					continue;

				supply_cfm_sensor_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::ExhaustCFM:
				if( !exhaust_cfm_sensor_ )
					continue;

				exhaust_cfm_sensor_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::SupplyRPM:
				if( !supply_rpm_sensor_ )
					continue;

				supply_rpm_sensor_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::ExhaustRPM:
				if( !exhaust_rpm_sensor_ )
					continue;

				exhaust_rpm_sensor_->publish_state(pField->m_value.m_flValue);
			break;
				
			case BroanField::TemperatureOut:
			{
				// @todo: We should stop querying NaN fields...
				if( !temperature_out_sensor_ || std::isnan( pField->m_value.m_flValue ) )
					continue;

				temperature_out_sensor_->publish_state(pField->m_value.m_flValue);		
			}
			break;

#endif	
#ifdef USE_NUMBER
			case BroanField::TargetHumidityA:
				if( !humidity_setpoint_number_ )
					continue;
				humidity_setpoint_number_->publish_state(pField->m_value.m_flValue);
			break;

			// @todo: We don't support unbalanced values here currently....
			case BroanField::CFMIn_Medium:
				if( !target_supply_cfm_medium_number_ )
					continue;

				target_supply_cfm_medium_number_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::CFMOut_Medium:
				if( !target_exhaust_cfm_medium_number_ )
					continue;

				target_exhaust_cfm_medium_number_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::CFMIn_Max:
				if( !target_supply_cfm_max_number_ )
					continue;

				target_supply_cfm_max_number_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::CFMOut_Max:
				if( !target_exhaust_cfm_max_number_ )
					continue;

				target_exhaust_cfm_max_number_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::CFMIn_Min:
				if( !target_supply_cfm_min_number_ )
					continue;

				target_supply_cfm_min_number_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::CFMOut_Min:
				if( !target_exhaust_cfm_min_number_ )
					continue;

				target_exhaust_cfm_min_number_->publish_state(pField->m_value.m_flValue);
			break;

			case BroanField::IntModeDuration:
				if( !intermittent_period_number_ )
					continue;

				intermittent_period_number_->publish_state(pField->m_value.m_nValue /* / 1000 */ );
			break;
#endif
#ifdef USE_SWITCH
			case BroanField::HumidityControl:
				if( !humidity_control_switch_ )
						continue;
				
				humidity_control_switch_->publish_state(pField->m_value.m_chValue==1);
			break;
	#endif
		}

		switch( pField->m_nType )
		{
			case BroanFieldType::Byte:
				ESP_LOGD("broan","%02X%02X is now Byte %02X", nOpcodeHigh, nOpcodeLow, pField->m_value.m_chValue );
				break;
			case BroanFieldType::Int:
				ESP_LOGD("broan","%02X%02X is now Int %i", nOpcodeHigh, nOpcodeLow, pField->m_value.m_nValue );
				break;
			case BroanFieldType::Float:
				ESP_LOGD("broan","%02X%02X is now Float %f", nOpcodeHigh, nOpcodeLow, pField->m_value.m_flValue );
				break;
			case BroanFieldType::Void:
				ESP_LOGD("broan","%02X%02X is not set", nOpcodeHigh, nOpcodeLow );
				break;
		}
    }

}

void BroanComponent::handleUnknownField(uint32_t nOpcodeHigh, uint32_t nOpcodeLow, uint8_t len, uint32_t i, const std::vector<uint8_t>& message )
{
#ifdef SCAN_UNKNOWN
	uint16_t kv = ( nOpcodeHigh << 8 ) | nOpcodeLow;
	if( m_vecFieldData.contains( kv ) )
	{
		BroanField_t copy = m_vecFieldData[ kv ];

		for (size_t b = 0; b < len && b < 4; ++b)
			m_vecFieldData[kv].m_value.m_rgBytes[b] = static_cast<char>(message[i+b]);

		if( m_vecFieldData[kv].m_value.m_nValue != copy.m_value.m_nValue )
		{


			if( len == 4)
				ESP_LOGD("broan","%02X%02X field is unmapped. Value: %f / %i -->  %f / %i", nOpcodeHigh, nOpcodeLow,
					copy.m_value.m_flValue, copy.m_value.m_nValue,
					m_vecFieldData[kv].m_value.m_flValue, m_vecFieldData[kv].m_value.m_nValue ) ;
			else if (len == 1)
				ESP_LOGD("broan","%02X%02X field is unmapped. Value: %f / %i -->  %f / %i", nOpcodeHigh, nOpcodeLow,
					copy.m_value.m_flValue, copy.m_value.m_nValue,
					m_vecFieldData[kv].m_value.m_flValue, m_vecFieldData[kv].m_value.m_nValue ) ;
		}
	}
	else
#endif
	{
		BroanField_t newField;
		newField.m_nOpcodeHigh = nOpcodeHigh;
		newField.m_nOpcodeLow = nOpcodeLow;
		newField.m_nType = len == 4 ? BroanFieldType::Float : BroanFieldType::Byte;

		for (size_t b = 0; b < len && b < 4; ++b)
			newField.m_value.m_rgBytes[b] = static_cast<char>(message[i+b]);


		if( len == 4)
			ESP_LOGD("broan","%02X%02X field is unmapped. Value: %f / %i", nOpcodeHigh, nOpcodeLow, newField.m_value.m_flValue, newField.m_value.m_nValue );
		else if( len == 1 )
			ESP_LOGD("broan","%02X%02X field is unmapped. Value: %i", nOpcodeHigh, nOpcodeLow, newField.m_value.m_chValue);
		else
			ESP_LOGD("broan","%02X%02X has unhandled field length %i: %s", nOpcodeHigh, nOpcodeLow, len, format_hex_pretty(&message[i], len).c_str() );
#ifdef SCAN_UNKNOWN
		m_vecFieldData[kv] = newField;
#endif

	}

}

void BroanComponent::send(const std::vector<uint8_t>& vecMessage)
{
#ifndef LISTEN_ONLY
	if( vecMessage.size() > MAX_FRAME_PAYLOAD_SIZE )
	{
		ESP_LOGE("broan", "Refusing to send oversized message: %i bytes", static_cast<int>( vecMessage.size() ) );
		return;
	}

	std::vector<uint8_t> frame;
	frame.reserve( 5 + vecMessage.size() + 2 );
	frame.push_back( 0x01 );
	frame.push_back( m_nServerAddress );
	frame.push_back( m_nClientAddress );
	frame.push_back( 0x01 );
	frame.push_back( static_cast<uint8_t>( vecMessage.size() ) );
	frame.insert( frame.end(), vecMessage.begin(), vecMessage.end() );
	frame.push_back( calculateChecksum( m_nClientAddress, m_nServerAddress, vecMessage ) );
	frame.push_back( 0x04 );

	writeRawToHrv( frame );
#endif
}

void BroanComponent::writeRawToHrv(const std::vector<uint8_t>& frame)
{
	writeRaw( this->parent_, this->flow_control_pin_, frame );
}

void BroanComponent::writeRawToRemote(const std::vector<uint8_t>& frame)
{
	writeRaw( this->remote_uart_, this->remote_flow_control_pin_, frame );
}

void BroanComponent::writeRaw(uart::UARTComponent *uart, GPIOPin *flow_control_pin, const std::vector<uint8_t>& frame)
{
#ifndef LISTEN_ONLY
	if( !uart || frame.empty() )
		return;

 	if(flow_control_pin)
    	flow_control_pin->digital_write(true);

	uart->write_array( frame );
	uart->flush();

 	if(flow_control_pin)
    	flow_control_pin->digital_write(false);
#endif
}

uint8_t BroanComponent::calculateChecksum(uint8_t sender, uint8_t receiver, const std::vector<uint8_t>& message)
{
	uint8_t total = 0x01 + sender + receiver + 0x01 + message.size();
	for (uint8_t b : message) total += b;
	return 0xFF & (0 - (total - 1));
}

BroanField_t* BroanComponent::lookupField( uint8_t opcodeHigh, uint8_t opcodeLow )
{
	uint32_t unField = lookupFieldIndex( opcodeHigh, opcodeLow );
	if( unField != INVALID_FIELD )
	{
		return &m_vecFields[unField];
	}

	return nullptr;
}

uint32_t BroanComponent::lookupFieldIndex( uint8_t opcodeHigh, uint8_t opcodeLow )
{
	for( int i=0; i<BroanField::MAX_FIELDS; i++ )
	{
		BroanField_t *pField = &m_vecFields[i];
		if( pField->m_nOpcodeHigh == opcodeHigh && pField->m_nOpcodeLow == opcodeLow )
			return i;
	}

	return INVALID_FIELD;
}

void BroanComponent::runTasks()
{
	uint32_t time = millis();

	if( m_bERVReady )
	{
		//ESP_LOGD("broan", "Reading values" );

		std::vector<unsigned char> vecRequest;

		int nCount = 0;
		for( int i=0; i<BroanField::MAX_FIELDS && nCount < MAX_REQUEST_SIZE; i++ )
		{
			if( m_vecFields[i].m_unPollRate == UPDATE_RATE_NEVER || time - m_vecFields[i].m_unLastUpdate < m_vecFields[i].m_unPollRate )
				continue;

			nCount++;
			m_vecFields[i].m_unLastUpdate = time;

			if( vecRequest.size() == 0 )
				vecRequest.push_back(0x20);

			vecRequest.push_back( m_vecFields[i].m_nOpcodeHigh );
			vecRequest.push_back( m_vecFields[i].m_nOpcodeLow );
		}

		if( vecRequest.size() > 0 )
		{
			queueMessage(vecRequest);
		}
	}


	if( !isPassThroughEnabled() && time - m_unLastHeartbeat > HEARTBEAT_RATE )
	{
		m_unLastHeartbeat = time;
		std::vector<unsigned char> vecRequest;
		vecRequest.push_back(0x40);
		vecRequest.push_back(0x00);
		vecRequest.push_back(0x50);
		vecRequest.push_back(0x00);

		queueMessage(vecRequest);
	}

#ifdef SCAN_UNKNOWN

	if( m_nNextScan == 0 )
		m_nNextScan	= time + 15000;

	if( m_bERVReady && time > m_nNextScan )
	{
		m_nNextScan = time + 100;

		std::vector<unsigned char> vecRequest;
		vecRequest.push_back(0x20);

		for( int i=0; i<15;i++)
		{
			vecRequest.push_back(m_nFieldCursor);
			vecRequest.push_back(m_nGroupCursor);


			if( m_nFieldCursor == 0xFF)
			{

				switch( m_nGroupCursor )
				{
					case 0x20: m_nGroupCursor = 0x21; break;
					case 0x21: m_nGroupCursor = 0x22; break;
					case 0x22: m_nGroupCursor = 0x30; break;
					case 0x30: m_nGroupCursor = 0x40; break;
					case 0x40: m_nGroupCursor = 0x50; break;
					case 0x50: m_nGroupCursor = 0x60; break;
					case 0x60: m_nGroupCursor = 0xE0; break;
					case 0xE0: m_nGroupCursor = 0x20; break;
					//case 0xF0: m_nGroupCursor = 0x20; break;
				}
				//ESP_LOGD("broan","Brute force: Group is now %02X ", m_nGroupCursor );
			}
			m_nFieldCursor++;
		}

		queueMessage(vecRequest);

	}
#endif
}



}
}
