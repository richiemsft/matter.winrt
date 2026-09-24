#pragma once

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>

#include "AttributePath.g.h"
#include "AttributeReportEventArgs.g.h"
#include "AttributeSubscription.g.h"
#include "AttributeValue.g.h"
#include "BasicInformation.g.h"
#include "BasicInformationCluster.g.h"
#include "BleCommissioningParameters.g.h"
#include "BleNetworkCommissioningParameters.g.h"
#include "CommandPath.g.h"
#include "CommandResult.g.h"
#include "CommissionedNode.g.h"
#include "CommissioningProgressEventArgs.g.h"
#include "ControllerOptions.g.h"
#include "EventPath.g.h"
#include "EventReportEventArgs.g.h"
#include "EventSubscription.g.h"
#include "EventValue.g.h"
#include "LevelControlCluster.g.h"
#include "MatterController.g.h"
#include "MatterControllerCommissioning.g.h"
#include "MatterControllerRecovery.g.h"
#include "MatterControllerNetworkCommissioning.g.h"
#include "MatterCommissioningProgress.g.h"
#include "MatterCommissioningResult.g.h"
#include "MatterNetworkInterface.g.h"
#include "MatterNetworkInterfaceProvider.g.h"
#include "MatterNetworkInterfaceSelection.g.h"
#include "OnNetworkCommissioningParameters.g.h"
#include "OnOffCluster.g.h"
#include "TimedInteractionOptions.g.h"
#include "ThreadNetworkCredentials.g.h"
#include "WiFiNetworkCredentials.g.h"

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace winrt::Matter::Windows::Controller::implementation {

namespace Windows = ::winrt::Windows;

class ControllerRuntime;

struct ControllerOptions : ControllerOptionsT<ControllerOptions>
{
    hstring StoragePath() const;
    void StoragePath(hstring const & value);
    uint64_t ControllerNodeId() const;
    void ControllerNodeId(uint64_t value);
    uint32_t BluetoothAdapterId() const;
    void BluetoothAdapterId(uint32_t value);
    bool AllowTestAttestation() const;
    void AllowTestAttestation(bool value);

private:
    hstring mStoragePath;
    uint64_t mControllerNodeId = 112233;
    uint32_t mBluetoothAdapterId = 0;
    bool mAllowTestAttestation = true;
};

struct OnNetworkCommissioningParameters : OnNetworkCommissioningParametersT<OnNetworkCommissioningParameters>
{
    uint64_t NodeId() const;
    void NodeId(uint64_t value);
    hstring SetupCode() const;
    void SetupCode(hstring const & value);
    uint32_t SetupPinCode() const;
    void SetupPinCode(uint32_t value);
    uint16_t LongDiscriminator() const;
    void LongDiscriminator(uint16_t value);
    hstring IpAddress() const;
    void IpAddress(hstring const & value);

private:
    uint64_t mNodeId = 1;
    hstring mSetupCode;
    uint32_t mSetupPinCode = 20202021;
    uint16_t mLongDiscriminator = 3840;
    hstring mIpAddress;
};

struct BleCommissioningParameters : BleCommissioningParametersT<BleCommissioningParameters>
{
    uint64_t NodeId() const;
    void NodeId(uint64_t value);
    uint32_t SetupPinCode() const;
    void SetupPinCode(uint32_t value);
    uint16_t LongDiscriminator() const;
    void LongDiscriminator(uint16_t value);

private:
    uint64_t mNodeId = 1;
    uint32_t mSetupPinCode = 20202021;
    uint16_t mLongDiscriminator = 3840;
};

struct WiFiNetworkCredentials : WiFiNetworkCredentialsT<WiFiNetworkCredentials>
{
    WiFiNetworkCredentials(hstring const & ssid, hstring const & passphrase);
    ~WiFiNetworkCredentials();
    uint32_t SsidLength() const;
    uint32_t PassphraseLength() const;
    std::vector<uint8_t> const & Ssid() const;
    std::vector<uint8_t> const & Passphrase() const;

private:
    std::vector<uint8_t> mSsid;
    std::vector<uint8_t> mPassphrase;
};

struct ThreadNetworkCredentials : ThreadNetworkCredentialsT<ThreadNetworkCredentials>
{
    explicit ThreadNetworkCredentials(Windows::Storage::Streams::IBuffer const & operationalDataset);
    ~ThreadNetworkCredentials();
    uint32_t DatasetLength() const;
    std::vector<uint8_t> const & OperationalDataset() const;

private:
    std::vector<uint8_t> mOperationalDataset;
};

struct BleNetworkCommissioningParameters : BleNetworkCommissioningParametersT<BleNetworkCommissioningParameters>
{
    uint64_t NodeId() const;
    void NodeId(uint64_t value);
    uint32_t SetupPinCode() const;
    void SetupPinCode(uint32_t value);
    uint16_t LongDiscriminator() const;
    void LongDiscriminator(uint16_t value);
    Controller::WiFiNetworkCredentials WiFi() const;
    void WiFi(Controller::WiFiNetworkCredentials const & value);
    Controller::ThreadNetworkCredentials Thread() const;
    void Thread(Controller::ThreadNetworkCredentials const & value);

private:
    uint64_t mNodeId = 1;
    uint32_t mSetupPinCode = 20202021;
    uint16_t mLongDiscriminator = 3840;
    Controller::WiFiNetworkCredentials mWiFi{ nullptr };
    Controller::ThreadNetworkCredentials mThread{ nullptr };
};

struct AttributePath : AttributePathT<AttributePath>
{
    AttributePath(uint16_t endpointId, uint32_t clusterId, uint32_t attributeId);
    uint16_t EndpointId() const;
    uint32_t ClusterId() const;
    uint32_t AttributeId() const;

private:
    uint16_t mEndpointId;
    uint32_t mClusterId;
    uint32_t mAttributeId;
};

struct CommandPath : CommandPathT<CommandPath>
{
    CommandPath(uint16_t endpointId, uint32_t clusterId, uint32_t commandId);
    uint16_t EndpointId() const;
    uint32_t ClusterId() const;
    uint32_t CommandId() const;

private:
    uint16_t mEndpointId;
    uint32_t mClusterId;
    uint32_t mCommandId;
};

struct EventPath : EventPathT<EventPath>
{
    EventPath(uint16_t endpointId, uint32_t clusterId, uint32_t eventId, bool urgent);
    uint16_t EndpointId() const;
    uint32_t ClusterId() const;
    uint32_t EventId() const;
    bool Urgent() const;

private:
    uint16_t mEndpointId;
    uint32_t mClusterId;
    uint32_t mEventId;
    bool mUrgent;
};

struct CommissionedNode : CommissionedNodeT<CommissionedNode>
{
    CommissionedNode(uint64_t nodeId, uint16_t fabricIndex);
    uint64_t NodeId() const;
    uint16_t FabricIndex() const;

private:
    uint64_t mNodeId;
    uint16_t mFabricIndex;
};

struct CommissioningProgressEventArgs : CommissioningProgressEventArgsT<CommissioningProgressEventArgs>
{
    CommissioningProgressEventArgs(Controller::CommissioningStage stage, hstring message);
    Controller::CommissioningStage Stage() const;
    hstring Message() const;

private:
    Controller::CommissioningStage mStage;
    hstring mMessage;
};

struct MatterNetworkInterfaceSelection : MatterNetworkInterfaceSelectionT<MatterNetworkInterfaceSelection>
{
    MatterNetworkInterfaceSelection(Controller::MatterNetworkInterfaceSelectionMode mode, uint64_t interfaceId);
    Controller::MatterNetworkInterfaceSelectionMode Mode() const;
    uint64_t InterfaceId() const;

private:
    Controller::MatterNetworkInterfaceSelectionMode mMode;
    uint64_t mInterfaceId;
};

struct MatterNetworkInterface : MatterNetworkInterfaceT<MatterNetworkInterface>
{
    MatterNetworkInterface(uint64_t id, uint32_t index, hstring name, Controller::MatterNetworkInterfaceType type,
                           bool connected, bool supportsIpv6, bool supportsMulticast, bool isVirtual);
    uint64_t Id() const;
    uint32_t InterfaceIndex() const;
    hstring Name() const;
    Controller::MatterNetworkInterfaceType Type() const;
    bool IsConnected() const;
    bool SupportsIpv6() const;
    bool SupportsMulticast() const;
    bool IsVirtual() const;

private:
    uint64_t mId;
    uint32_t mIndex;
    hstring mName;
    Controller::MatterNetworkInterfaceType mType;
    bool mConnected;
    bool mSupportsIpv6;
    bool mSupportsMulticast;
    bool mIsVirtual;
};

struct MatterCommissioningProgress : MatterCommissioningProgressT<MatterCommissioningProgress>
{
    MatterCommissioningProgress(Controller::MatterCommissioningStage stage,
                                Controller::MatterCommissioningStage lastCompletedStage, int32_t nativeStageId,
                                Controller::MatterCommissioningTransport transport, Windows::Foundation::TimeSpan elapsedTime,
                                hstring diagnosticMessage, hstring displayMessage, uint64_t networkInterfaceId,
                                hstring networkInterfaceName, uint32_t attemptNumber, bool isRetrying);
    Controller::MatterCommissioningStage Stage() const;
    Controller::MatterCommissioningStage LastCompletedStage() const;
    int32_t NativeStageId() const;
    Controller::MatterCommissioningTransport Transport() const;
    Windows::Foundation::TimeSpan ElapsedTime() const;
    hstring DiagnosticMessage() const;
    hstring DisplayMessage() const;
    uint64_t NetworkInterfaceId() const;
    hstring NetworkInterfaceName() const;
    uint32_t AttemptNumber() const;
    bool IsRetrying() const;

private:
    Controller::MatterCommissioningStage mStage;
    Controller::MatterCommissioningStage mLastCompletedStage;
    int32_t mNativeStageId;
    Controller::MatterCommissioningTransport mTransport;
    Windows::Foundation::TimeSpan mElapsedTime;
    hstring mDiagnosticMessage;
    hstring mDisplayMessage;
    uint64_t mNetworkInterfaceId;
    hstring mNetworkInterfaceName;
    uint32_t mAttemptNumber;
    bool mIsRetrying;
};

struct MatterCommissioningResult : MatterCommissioningResultT<MatterCommissioningResult>
{
    MatterCommissioningResult(bool succeeded, Controller::MatterCommissioningOutcome outcome,
                              Controller::MatterCommissioningFailureKind failureKind,
                              Controller::MatterCommissioningStage failedStage,
                              Controller::MatterCommissioningStage lastCompletedStage, int32_t nativeStageId,
                              int32_t nativeErrorCode, hstring diagnosticMessage, uint64_t networkInterfaceId,
                              Controller::CommissionedNode node);
    bool Succeeded() const;
    Controller::MatterCommissioningOutcome Outcome() const;
    Controller::MatterCommissioningFailureKind FailureKind() const;
    Controller::MatterCommissioningStage FailedStage() const;
    Controller::MatterCommissioningStage LastCompletedStage() const;
    int32_t NativeStageId() const;
    int32_t NativeErrorCode() const;
    hstring DiagnosticMessage() const;
    uint64_t NetworkInterfaceId() const;
    Controller::CommissionedNode Node() const;

private:
    bool mSucceeded;
    Controller::MatterCommissioningOutcome mOutcome;
    Controller::MatterCommissioningFailureKind mFailureKind;
    Controller::MatterCommissioningStage mFailedStage;
    Controller::MatterCommissioningStage mLastCompletedStage;
    int32_t mNativeStageId;
    int32_t mNativeErrorCode;
    hstring mDiagnosticMessage;
    uint64_t mNetworkInterfaceId;
    Controller::CommissionedNode mNode{ nullptr };
};

struct AttributeValue : AttributeValueT<AttributeValue>
{
    AttributeValue(Controller::AttributePath path, Windows::Foundation::Collections::IPropertySet data);
    Controller::AttributePath Path() const;
    Windows::Foundation::Collections::IPropertySet Data() const;

private:
    Controller::AttributePath mPath;
    Windows::Foundation::Collections::IPropertySet mData;
};

struct CommandResult : CommandResultT<CommandResult>
{
    CommandResult(Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet data);
    Controller::CommandPath Path() const;
    Windows::Foundation::Collections::IPropertySet Data() const;

private:
    Controller::CommandPath mPath;
    Windows::Foundation::Collections::IPropertySet mData;
};

struct TimedInteractionOptions : TimedInteractionOptionsT<TimedInteractionOptions>
{
    explicit TimedInteractionOptions(uint16_t timeoutMilliseconds);
    uint16_t TimeoutMilliseconds() const;

private:
    uint16_t mTimeoutMilliseconds;
};

struct EventValue : EventValueT<EventValue>
{
    EventValue(Controller::EventPath path, uint64_t eventNumber, Windows::Foundation::Collections::IPropertySet data);
    Controller::EventPath Path() const;
    uint64_t EventNumber() const;
    Windows::Foundation::Collections::IPropertySet Data() const;

private:
    Controller::EventPath mPath;
    uint64_t mEventNumber;
    Windows::Foundation::Collections::IPropertySet mData;
};

struct EventReportEventArgs : EventReportEventArgsT<EventReportEventArgs>
{
    explicit EventReportEventArgs(Controller::EventValue value);
    Controller::EventValue Value() const;

private:
    Controller::EventValue mValue;
};

struct EventSubscription : EventSubscriptionT<EventSubscription>
{
    EventSubscription() = default;
    explicit EventSubscription(std::function<void()> close);
    event_token ReportReceived(
        Windows::Foundation::TypedEventHandler<Controller::EventSubscription, Controller::EventReportEventArgs> const & handler);
    void ReportReceived(event_token const & token) noexcept;
    Windows::Foundation::IAsyncAction CloseAsync();
    void Publish(Controller::EventReportEventArgs const & args);

private:
    winrt::event<Windows::Foundation::TypedEventHandler<Controller::EventSubscription, Controller::EventReportEventArgs>>
        mReportReceived;
    std::mutex mReportMutex;
    std::vector<Controller::EventReportEventArgs> mPendingReports;
    size_t mReportHandlerCount = 0;
    bool mDrainingPendingReports = false;
    std::function<void()> mClose;
};

struct AttributeReportEventArgs : AttributeReportEventArgsT<AttributeReportEventArgs>
{
    explicit AttributeReportEventArgs(Controller::AttributeValue value);
    Controller::AttributeValue Value() const;

private:
    Controller::AttributeValue mValue;
};

struct AttributeSubscription : AttributeSubscriptionT<AttributeSubscription>
{
    AttributeSubscription() = default;
    explicit AttributeSubscription(std::function<void()> close);
    event_token ReportReceived(
        Windows::Foundation::TypedEventHandler<Controller::AttributeSubscription, Controller::AttributeReportEventArgs> const & handler);
    void ReportReceived(event_token const & token) noexcept;
    Windows::Foundation::IAsyncAction CloseAsync();
    void Publish(Controller::AttributeReportEventArgs const & args);

private:
    winrt::event<Windows::Foundation::TypedEventHandler<Controller::AttributeSubscription, Controller::AttributeReportEventArgs>>
        mReportReceived;
    std::mutex mReportMutex;
    std::vector<Controller::AttributeReportEventArgs> mPendingReports;
    size_t mReportHandlerCount = 0;
    bool mDrainingPendingReports = false;
    std::function<void()> mClose;
};

struct BasicInformation : BasicInformationT<BasicInformation>
{
    BasicInformation(uint16_t vendorId, hstring vendorName, uint16_t productId, hstring productName, hstring nodeLabel,
                     hstring serialNumber, uint32_t softwareVersion, hstring softwareVersionString);
    uint16_t VendorId() const;
    hstring VendorName() const;
    uint16_t ProductId() const;
    hstring ProductName() const;
    hstring NodeLabel() const;
    hstring SerialNumber() const;
    uint32_t SoftwareVersion() const;
    hstring SoftwareVersionString() const;

private:
    uint16_t mVendorId;
    hstring mVendorName;
    uint16_t mProductId;
    hstring mProductName;
    hstring mNodeLabel;
    hstring mSerialNumber;
    uint32_t mSoftwareVersion;
    hstring mSoftwareVersionString;
};

struct OnOffCluster : OnOffClusterT<OnOffCluster>
{
    OnOffCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncOperation<bool> ReadAsync();
    Windows::Foundation::IAsyncAction SetAsync(bool value);
    Windows::Foundation::IAsyncAction OnAsync();
    Windows::Foundation::IAsyncAction OffAsync();
    Windows::Foundation::IAsyncAction ToggleAsync();
    Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription> SubscribeAsync(uint16_t minimumIntervalSeconds,
                                                                                           uint16_t maximumIntervalSeconds);

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
    uint64_t mNodeId;
    uint16_t mEndpointId;
};

struct LevelControlCluster : LevelControlClusterT<LevelControlCluster>
{
    LevelControlCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncOperation<uint8_t> ReadCurrentLevelAsync();
    Windows::Foundation::IAsyncAction MoveToLevelAsync(uint8_t level, uint16_t transitionTime, uint8_t optionsMask,
                                                       uint8_t optionsOverride);
    Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription> SubscribeAsync(uint16_t minimumIntervalSeconds,
                                                                                           uint16_t maximumIntervalSeconds);

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
    uint64_t mNodeId;
    uint16_t mEndpointId;
};

struct BasicInformationCluster : BasicInformationClusterT<BasicInformationCluster>
{
    BasicInformationCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncOperation<Controller::BasicInformation> ReadAsync();

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
    uint64_t mNodeId;
    uint16_t mEndpointId;
};

struct MatterController : MatterControllerT<MatterController>
{
    explicit MatterController(std::shared_ptr<ControllerRuntime> runtime);
    static Windows::Foundation::IAsyncOperation<Controller::MatterController> CreateAsync(Controller::ControllerOptions options);
    event_token CommissioningProgress(
        Windows::Foundation::TypedEventHandler<Controller::MatterController, Controller::CommissioningProgressEventArgs> const & handler);
    void CommissioningProgress(event_token const & token) noexcept;
    Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
    CommissionOnNetworkAsync(Controller::OnNetworkCommissioningParameters parameters);
    Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
    CommissionBleAsync(Controller::BleCommissioningParameters parameters);
    Windows::Foundation::Collections::IVectorView<Controller::CommissionedNode> CommissionedNodes();
    Windows::Foundation::IAsyncAction RemoveNodeAsync(uint64_t nodeId);
    Windows::Foundation::IAsyncOperation<Controller::AttributeValue> ReadAttributeAsync(uint64_t nodeId,
                                                                                        Controller::AttributePath path);
    Windows::Foundation::IAsyncAction WriteAttributeAsync(uint64_t nodeId, Controller::AttributePath path,
                                                          Windows::Foundation::Collections::IPropertySet value);
    Windows::Foundation::IAsyncAction WriteAttributeTimedAsync(
        uint64_t nodeId, Controller::AttributePath path, Windows::Foundation::Collections::IPropertySet value,
        Controller::TimedInteractionOptions options);
    Windows::Foundation::IAsyncOperation<Controller::CommandResult>
    InvokeCommandAsync(uint64_t nodeId, Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet arguments);
    Windows::Foundation::IAsyncOperation<Controller::CommandResult> InvokeCommandTimedAsync(
        uint64_t nodeId, Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet arguments,
        Controller::TimedInteractionOptions options);
    Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription>
    SubscribeAttributeAsync(uint64_t nodeId, Controller::AttributePath path, uint16_t minimumIntervalSeconds,
                            uint16_t maximumIntervalSeconds);
    Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::EventValue>>
    ReadEventsAsync(uint64_t nodeId, Controller::EventPath path, uint64_t minimumEventNumber);
    Windows::Foundation::IAsyncOperation<Controller::EventSubscription>
    SubscribeEventAsync(uint64_t nodeId, Controller::EventPath path, uint64_t minimumEventNumber,
                        uint16_t minimumIntervalSeconds, uint16_t maximumIntervalSeconds);
    Controller::OnOffCluster GetOnOffCluster(uint64_t nodeId, uint16_t endpointId);
    Controller::LevelControlCluster GetLevelControlCluster(uint64_t nodeId, uint16_t endpointId);
    Controller::BasicInformationCluster GetBasicInformationCluster(uint64_t nodeId, uint16_t endpointId);
    Windows::Foundation::IAsyncAction CloseAsync();
    std::shared_ptr<ControllerRuntime> Runtime();

private:
    std::mutex mRuntimeMutex;
    std::shared_ptr<ControllerRuntime> mRuntime;
    winrt::event<Windows::Foundation::TypedEventHandler<Controller::MatterController, Controller::CommissioningProgressEventArgs>>
        mCommissioningProgress;
};

struct MatterControllerRecovery : MatterControllerRecoveryT<MatterControllerRecovery>
{
    explicit MatterControllerRecovery(Controller::MatterController controller);
    Windows::Foundation::IAsyncOperation<Controller::CommissionedNode> RecoverNodeAsync(uint64_t nodeId);

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
};

struct MatterControllerNetworkCommissioning : MatterControllerNetworkCommissioningT<MatterControllerNetworkCommissioning>
{
    explicit MatterControllerNetworkCommissioning(Controller::MatterController controller);
    Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
    CommissionBleAsync(Controller::BleNetworkCommissioningParameters parameters);

private:
    std::shared_ptr<ControllerRuntime> mRuntime;
};

struct MatterNetworkInterfaceProvider
{
    static Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::MatterNetworkInterface>>
    GetEligibleNetworkInterfacesAsync();
};

struct MatterControllerCommissioning : MatterControllerCommissioningT<MatterControllerCommissioning>
{
    explicit MatterControllerCommissioning(Controller::MatterController controller);
    event_token ProgressChanged(
        Windows::Foundation::TypedEventHandler<Controller::MatterControllerCommissioning,
                                               Controller::MatterCommissioningProgress> const & handler);
    void ProgressChanged(event_token const & token) noexcept;
    Windows::Foundation::IAsyncOperation<Controller::MatterCommissioningResult>
    CommissionBleAsync(Controller::BleNetworkCommissioningParameters parameters,
                       Controller::MatterNetworkInterfaceSelection interfaceSelection);

private:
    void Publish(Controller::MatterCommissioningProgress const & progress);
    void DrainProgress();
    void WaitForProgressDrain();

    std::shared_ptr<ControllerRuntime> mRuntime;
    winrt::event<Windows::Foundation::TypedEventHandler<Controller::MatterControllerCommissioning,
                                                        Controller::MatterCommissioningProgress>>
        mProgressChanged;
    std::mutex mProgressMutex;
    std::condition_variable mProgressCondition;
    std::vector<Controller::MatterCommissioningProgress> mPendingProgress;
    bool mProgressDrainScheduled = false;
};

} // namespace winrt::Matter::Windows::Controller::implementation

namespace winrt::Matter::Windows::Controller::factory_implementation {

struct ControllerOptions : ControllerOptionsT<ControllerOptions, implementation::ControllerOptions>
{};
struct OnNetworkCommissioningParameters :
    OnNetworkCommissioningParametersT<OnNetworkCommissioningParameters, implementation::OnNetworkCommissioningParameters>
{};
struct BleCommissioningParameters :
    BleCommissioningParametersT<BleCommissioningParameters, implementation::BleCommissioningParameters>
{};
struct BleNetworkCommissioningParameters :
    BleNetworkCommissioningParametersT<BleNetworkCommissioningParameters, implementation::BleNetworkCommissioningParameters>
{};
struct WiFiNetworkCredentials :
    WiFiNetworkCredentialsT<WiFiNetworkCredentials, implementation::WiFiNetworkCredentials>
{};
struct ThreadNetworkCredentials :
    ThreadNetworkCredentialsT<ThreadNetworkCredentials, implementation::ThreadNetworkCredentials>
{};
struct AttributePath : AttributePathT<AttributePath, implementation::AttributePath>
{};
struct CommandPath : CommandPathT<CommandPath, implementation::CommandPath>
{};
struct EventPath : EventPathT<EventPath, implementation::EventPath>
{};
struct TimedInteractionOptions : TimedInteractionOptionsT<TimedInteractionOptions, implementation::TimedInteractionOptions>
{};
struct MatterController : MatterControllerT<MatterController, implementation::MatterController>
{};
struct MatterControllerRecovery : MatterControllerRecoveryT<MatterControllerRecovery, implementation::MatterControllerRecovery>
{};
struct MatterControllerNetworkCommissioning :
    MatterControllerNetworkCommissioningT<MatterControllerNetworkCommissioning,
                                          implementation::MatterControllerNetworkCommissioning>
{};
struct MatterNetworkInterfaceSelection :
    MatterNetworkInterfaceSelectionT<MatterNetworkInterfaceSelection, implementation::MatterNetworkInterfaceSelection>
{};
struct MatterNetworkInterfaceProvider :
    MatterNetworkInterfaceProviderT<MatterNetworkInterfaceProvider, implementation::MatterNetworkInterfaceProvider>
{};
struct MatterControllerCommissioning :
    MatterControllerCommissioningT<MatterControllerCommissioning, implementation::MatterControllerCommissioning>
{};

} // namespace winrt::Matter::Windows::Controller::factory_implementation
