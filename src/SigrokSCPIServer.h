#ifndef SigrokSCPIServer_h
#define SigrokSCPIServer_h

#include "scpi-server-tools/BridgeSCPIServer.h"
#include "log/log.h"

/**
	@brief SCPI server for managing control plane traffic to a single client
 */
class SigrokSCPIServer : public BridgeSCPIServer
{
public:
	SigrokSCPIServer(ZSOCKET sock);
	virtual ~SigrokSCPIServer();

protected:
	virtual std::string GetMake() override;
	virtual std::string GetModel() override;
	virtual std::string GetSerial() override;
	virtual std::string GetFirmwareVersion() override;
	virtual size_t GetAnalogChannelCount() override;
	virtual std::vector<size_t> GetSampleRates() override;
	virtual std::vector<size_t> GetSampleDepths() override;

	virtual bool OnCommand(
		const std::string& line,
		const std::string& subject,
		const std::string& cmd,
		const std::vector<std::string>& args) override;

	virtual bool OnQuery(
		const std::string& line,
		const std::string& subject,
		const std::string& cmd) override;

	virtual bool GetChannelID(const std::string& subject, size_t& id_out) override;
	virtual ChannelType GetChannelType(size_t channel) override;

	virtual void AcquisitionStart(bool oneShot = false) override;
	virtual void AcquisitionForceTrigger() override;
	virtual void AcquisitionStop() override;
	virtual bool IsTriggerArmed() override;
	virtual void SetChannelEnabled(size_t chIndex, bool enabled) override;
	virtual void SetAnalogCoupling(size_t chIndex, const std::string& coupling) override;
	virtual void SetAnalogRange(size_t chIndex, double range_V) override;
	virtual void SetAnalogOffset(size_t chIndex, double offset_V) override;
	virtual void SetDigitalThreshold(size_t chIndex, double threshold_V) override;
	virtual void SetDigitalHysteresis(size_t chIndex, double hysteresis) override;
	virtual void SetSampleRate(uint64_t rate_hz) override;
	virtual void SetSampleDepth(uint64_t depth) override;
	virtual void SetTriggerDelay(uint64_t delay_fs) override;
	virtual void SetTriggerSource(size_t chIndex) override;
	virtual void SetTriggerLevel(double level_V) override;
	virtual void SetTriggerTypeEdge() override;
	virtual void SetEdgeTriggerEdge(const std::string& edge) override;
};

#endif
