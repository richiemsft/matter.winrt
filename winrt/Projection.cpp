#include "ProjectionTypes.h"

#include <app-common/zap-generated/cluster-objects.h>
#include <app/CommandSender.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadClient.h>
#include <app/WriteClient.h>
#include <app/data-model/EncodableToTLV.h>
#include <app/server-cluster/testing/EmptyProvider.h>
#include <controller/CHIPCluster.h>
#include <controller/CHIPDeviceController.h>
#include <controller/CHIPDeviceControllerFactory.h>
#include <controller/ExampleOperationalCredentialsIssuer.h>
#include <controller/InvokeInteraction.h>
#include <credentials/GroupDataProviderImpl.h>
#include <credentials/PersistentStorageOpCertStore.h>
#include <credentials/attestation_verifier/DefaultDeviceAttestationVerifier.h>
#include <crypto/CHIPCryptoPAL.h>
#include <crypto/PersistentStorageOperationalKeystore.h>
#include <crypto/RawKeySessionKeystore.h>
#include <inet/InetInterface.h>
#include <lib/support/CHIPMem.h>
#include <lib/support/TestGroupData.h>
#include <lib/core/TLV.h>
#include <platform/CHIPDeviceLayer.h>
#include <platform/KvsPersistentStorageDelegate.h>
#include <platform/Windows/BLEManagerImpl.h>
#include <platform/Windows/ConfigurationManagerImpl.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/SetupPayload.h>
#include <winrt/Windows.System.Threading.h>

#include <chrono>
#include <algorithm>
#include <atomic>
#include <array>
#include <cctype>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace winrt::Matter::Windows::Controller::implementation {
namespace {

using namespace chip;
using namespace chip::Controller;
using namespace chip::DeviceLayer;

constexpr FabricId kControllerFabricId = 1;
constexpr auto kCommissioningTimeout    = std::chrono::seconds(120);
constexpr auto kCommissioningFailureDetailTimeout = std::chrono::milliseconds(100);
constexpr auto kInteractionTimeout      = std::chrono::seconds(30);
constexpr auto kTimedInteractionMargin  = std::chrono::seconds(5);
constexpr char kCommissionedNodesKey[] = "winrt/nodes";
constexpr size_t kGenericTlvBufferSize = 64 * 1024;
constexpr size_t kMaximumPendingReports = 256;

struct NativeCommissioningProgress
{
    chip::Controller::CommissioningStage stage;
    CHIP_ERROR error = CHIP_NO_ERROR;
    bool completed   = false;
    bool retrying    = false;
};

using CommissioningProgressCallback = std::function<void(const NativeCommissioningProgress &)>;

Controller::MatterCommissioningStage ProjectCommissioningStage(chip::Controller::CommissioningStage stage)
{
    switch (stage)
    {
    case chip::Controller::CommissioningStage::kSecurePairing:
        return Controller::MatterCommissioningStage::EstablishingPase;
    case chip::Controller::CommissioningStage::kReadCommissioningInfo:
        return Controller::MatterCommissioningStage::ReadingCommissioningInformation;
    case chip::Controller::CommissioningStage::kConfigRegulatory:
        return Controller::MatterCommissioningStage::ConfiguringRegulatoryInformation;
    case chip::Controller::CommissioningStage::kWiFiNetworkSetup:
    case chip::Controller::CommissioningStage::kThreadNetworkSetup:
    case chip::Controller::CommissioningStage::kRequestWiFiCredentials:
    case chip::Controller::CommissioningStage::kRequestThreadCredentials:
        return Controller::MatterCommissioningStage::ProvisioningNetwork;
    case chip::Controller::CommissioningStage::kWiFiNetworkEnable:
    case chip::Controller::CommissioningStage::kThreadNetworkEnable:
        return Controller::MatterCommissioningStage::ConnectingDeviceToNetwork;
    case chip::Controller::CommissioningStage::kFindOperationalForStayActive:
    case chip::Controller::CommissioningStage::kFindOperationalForCommissioningComplete:
        return Controller::MatterCommissioningStage::DiscoveringOperationalDevice;
    case chip::Controller::CommissioningStage::kSendComplete:
        return Controller::MatterCommissioningStage::SendingCommissioningComplete;
    default:
        return Controller::MatterCommissioningStage::Unknown;
    }
}

Controller::MatterCommissioningTransport ProjectCommissioningTransport(chip::Controller::CommissioningStage stage)
{
    if (stage == chip::Controller::CommissioningStage::kFindOperationalForStayActive ||
        stage == chip::Controller::CommissioningStage::kFindOperationalForCommissioningComplete ||
        stage == chip::Controller::CommissioningStage::kSendComplete)
    {
        return Controller::MatterCommissioningTransport::OperationalIp;
    }
    if (stage == chip::Controller::CommissioningStage::kWiFiNetworkSetup ||
        stage == chip::Controller::CommissioningStage::kWiFiNetworkEnable)
    {
        return Controller::MatterCommissioningTransport::WiFi;
    }
    if (stage == chip::Controller::CommissioningStage::kThreadNetworkSetup ||
        stage == chip::Controller::CommissioningStage::kThreadNetworkEnable)
    {
        return Controller::MatterCommissioningTransport::Thread;
    }
    return Controller::MatterCommissioningTransport::Bluetooth;
}

Controller::MatterCommissioningFailureKind ClassifyCommissioningFailure(Controller::MatterCommissioningStage stage,
                                                                        bool timedOut)
{
    switch (stage)
    {
    case Controller::MatterCommissioningStage::SearchingForDevice:
        return Controller::MatterCommissioningFailureKind::DeviceNotFoundOverBluetooth;
    case Controller::MatterCommissioningStage::ConnectingOverBluetooth:
        return Controller::MatterCommissioningFailureKind::BluetoothConnectionFailed;
    case Controller::MatterCommissioningStage::EstablishingPase:
        return Controller::MatterCommissioningFailureKind::PaseFailed;
    case Controller::MatterCommissioningStage::ProvisioningNetwork:
        return Controller::MatterCommissioningFailureKind::NetworkCredentialsRejected;
    case Controller::MatterCommissioningStage::ConnectingDeviceToNetwork:
        return Controller::MatterCommissioningFailureKind::DeviceNetworkConnectionFailed;
    case Controller::MatterCommissioningStage::DiscoveringOperationalDevice:
        return Controller::MatterCommissioningFailureKind::OperationalDiscoveryTimedOut;
    case Controller::MatterCommissioningStage::EstablishingCase:
        return Controller::MatterCommissioningFailureKind::CaseEstablishmentFailed;
    case Controller::MatterCommissioningStage::SendingCommissioningComplete:
        return Controller::MatterCommissioningFailureKind::CommissioningCompleteFailed;
    default:
        return timedOut ? Controller::MatterCommissioningFailureKind::OverallTimeout
                        : Controller::MatterCommissioningFailureKind::Unknown;
    }
}

std::vector<uint8_t> Utf8Bytes(hstring const & value)
{
    std::string encoded = to_string(value);
    std::vector<uint8_t> bytes(encoded.begin(), encoded.end());
    if (!encoded.empty())
    {
        Crypto::ClearSecretData(reinterpret_cast<uint8_t *>(encoded.data()), encoded.size());
    }
    return bytes;
}

std::chrono::milliseconds InteractionWaitTimeout(std::optional<uint16_t> timedInteractionTimeout)
{
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(kInteractionTimeout);
    if (timedInteractionTimeout.has_value())
    {
        timeout += std::chrono::milliseconds(*timedInteractionTimeout) + kTimedInteractionMargin;
    }
    return timeout;
}

[[noreturn]] void ThrowChipError(CHIP_ERROR error, wchar_t const * operation)
{
    std::wstring message(operation);
    message.append(L": ");
    message.append(to_hstring(error.AsString()).c_str());
    throw hresult_error(HRESULT_FROM_WIN32(ERROR_GEN_FAILURE), message);
}

void CheckChipError(CHIP_ERROR error, wchar_t const * operation)
{
    if (error != CHIP_NO_ERROR)
    {
        ThrowChipError(error, operation);
    }
}

CHIP_ERROR EncodeInspectable(TLV::TLVWriter & writer, TLV::Tag tag, Windows::Foundation::IInspectable const & value);

CHIP_ERROR EncodePropertySet(TLV::TLVWriter & writer, TLV::Tag tag,
                             Windows::Foundation::Collections::IPropertySet const & values)
{
    TLV::TLVType container;
    ReturnErrorOnFailure(writer.StartContainer(tag, TLV::kTLVType_Structure, container));
    std::vector<std::pair<uint8_t, Windows::Foundation::IInspectable>> fields;
    for (auto const & entry : values)
    {
        std::string key = to_string(entry.Key());
        char * end      = nullptr;
        unsigned long number = std::strtoul(key.c_str(), &end, 10);
        VerifyOrReturnError(end != key.c_str() && *end == '\0' && number <= UINT8_MAX, CHIP_ERROR_INVALID_ARGUMENT);
        fields.emplace_back(static_cast<uint8_t>(number), entry.Value());
    }
    std::sort(fields.begin(), fields.end(), [](auto const & left, auto const & right) { return left.first < right.first; });
    for (auto const & field : fields)
    {
        ReturnErrorOnFailure(EncodeInspectable(writer, TLV::ContextTag(field.first), field.second));
    }
    return writer.EndContainer(container);
}

CHIP_ERROR EncodeInspectable(TLV::TLVWriter & writer, TLV::Tag tag, Windows::Foundation::IInspectable const & value)
{
    if (!value)
    {
        return writer.PutNull(tag);
    }
    if (auto propertySet = value.try_as<Windows::Foundation::Collections::IPropertySet>())
    {
        return EncodePropertySet(writer, tag, propertySet);
    }
    if (auto inspectableVector = value.try_as<Windows::Foundation::Collections::IVector<Windows::Foundation::IInspectable>>())
    {
        TLV::TLVType container;
        ReturnErrorOnFailure(writer.StartContainer(tag, TLV::kTLVType_Array, container));
        for (auto const & item : inspectableVector)
        {
            ReturnErrorOnFailure(EncodeInspectable(writer, TLV::AnonymousTag(), item));
        }
        return writer.EndContainer(container);
    }
    if (auto byteVector = value.try_as<Windows::Foundation::Collections::IVector<uint8_t>>())
    {
        std::vector<uint8_t> bytes(byteVector.Size());
        byteVector.GetMany(0, bytes);
        return writer.PutBytes(tag, bytes.data(), static_cast<uint32_t>(bytes.size()));
    }

    auto propertyValue = value.try_as<Windows::Foundation::IPropertyValue>();
    VerifyOrReturnError(propertyValue, CHIP_ERROR_INVALID_ARGUMENT);
    switch (propertyValue.Type())
    {
    case Windows::Foundation::PropertyType::Boolean:
        return writer.Put(tag, propertyValue.GetBoolean());
    case Windows::Foundation::PropertyType::Int16:
        return writer.Put(tag, propertyValue.GetInt16());
    case Windows::Foundation::PropertyType::Int32:
        return writer.Put(tag, propertyValue.GetInt32());
    case Windows::Foundation::PropertyType::Int64:
        return writer.Put(tag, propertyValue.GetInt64());
    case Windows::Foundation::PropertyType::UInt8:
        return writer.Put(tag, propertyValue.GetUInt8());
    case Windows::Foundation::PropertyType::UInt16:
        return writer.Put(tag, propertyValue.GetUInt16());
    case Windows::Foundation::PropertyType::UInt32:
        return writer.Put(tag, propertyValue.GetUInt32());
    case Windows::Foundation::PropertyType::UInt64:
        return writer.Put(tag, propertyValue.GetUInt64());
    case Windows::Foundation::PropertyType::Single:
        return writer.Put(tag, propertyValue.GetSingle());
    case Windows::Foundation::PropertyType::Double:
        return writer.Put(tag, propertyValue.GetDouble());
    case Windows::Foundation::PropertyType::String: {
        std::string text = to_string(propertyValue.GetString());
        return writer.PutString(tag, CharSpan(text.data(), text.size()));
    }
    case Windows::Foundation::PropertyType::UInt8Array: {
        com_array<uint8_t> bytes;
        propertyValue.GetUInt8Array(bytes);
        return writer.PutBytes(tag, bytes.data(), static_cast<uint32_t>(bytes.size()));
    }
    default:
        return CHIP_ERROR_INVALID_ARGUMENT;
    }
}

CHIP_ERROR EncodePropertySetRoot(Windows::Foundation::Collections::IPropertySet const & values, std::vector<uint8_t> & encoded)
{
    VerifyOrReturnError(values, CHIP_ERROR_INVALID_ARGUMENT);
    encoded.resize(kGenericTlvBufferSize);
    TLV::TLVWriter writer;
    writer.Init(encoded.data(), encoded.size());
    if (values.HasKey(L"value"))
    {
        VerifyOrReturnError(values.Size() == 1, CHIP_ERROR_INVALID_ARGUMENT);
        ReturnErrorOnFailure(EncodeInspectable(writer, TLV::AnonymousTag(), values.Lookup(L"value")));
    }
    else
    {
        ReturnErrorOnFailure(EncodePropertySet(writer, TLV::AnonymousTag(), values));
    }
    ReturnErrorOnFailure(writer.Finalize());
    encoded.resize(writer.GetLengthWritten());
    return CHIP_NO_ERROR;
}

CHIP_ERROR DecodeTlvValue(TLV::TLVReader & reader, Windows::Foundation::IInspectable & value)
{
    switch (reader.GetType())
    {
    case TLV::kTLVType_Boolean: {
        bool decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        value = box_value(decoded);
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_SignedInteger: {
        int64_t decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        value = box_value(decoded);
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_UnsignedInteger: {
        uint64_t decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        value = box_value(decoded);
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_FloatingPointNumber: {
        double decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        value = box_value(decoded);
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_UTF8String: {
        CharSpan decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        value = box_value(to_hstring(std::string(decoded.data(), decoded.size())));
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_ByteString: {
        ByteSpan decoded;
        ReturnErrorOnFailure(reader.Get(decoded));
        std::vector<uint8_t> bytes(decoded.begin(), decoded.end());
        value = single_threaded_vector(std::move(bytes)).as<Windows::Foundation::IInspectable>();
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_Null:
        value = nullptr;
        return CHIP_NO_ERROR;
    case TLV::kTLVType_Structure: {
        Windows::Foundation::Collections::PropertySet properties;
        TLV::TLVType container;
        ReturnErrorOnFailure(reader.EnterContainer(container));
        CHIP_ERROR error;
        while ((error = reader.Next()) == CHIP_NO_ERROR)
        {
            VerifyOrReturnError(TLV::IsContextTag(reader.GetTag()), CHIP_ERROR_INVALID_TLV_TAG);
            Windows::Foundation::IInspectable field;
            ReturnErrorOnFailure(DecodeTlvValue(reader, field));
            properties.Insert(to_hstring(TLV::TagNumFromTag(reader.GetTag())), field);
        }
        VerifyOrReturnError(error == CHIP_END_OF_TLV, error);
        ReturnErrorOnFailure(reader.ExitContainer(container));
        value = properties;
        return CHIP_NO_ERROR;
    }
    case TLV::kTLVType_Array:
    case TLV::kTLVType_List: {
        auto items = single_threaded_vector<Windows::Foundation::IInspectable>();
        TLV::TLVType container;
        ReturnErrorOnFailure(reader.EnterContainer(container));
        CHIP_ERROR error;
        while ((error = reader.Next()) == CHIP_NO_ERROR)
        {
            Windows::Foundation::IInspectable item;
            ReturnErrorOnFailure(DecodeTlvValue(reader, item));
            items.Append(item);
        }
        VerifyOrReturnError(error == CHIP_END_OF_TLV, error);
        ReturnErrorOnFailure(reader.ExitContainer(container));
        value = items.as<Windows::Foundation::IInspectable>();
        return CHIP_NO_ERROR;
    }
    default:
        return CHIP_ERROR_WRONG_TLV_TYPE;
    }
}

Windows::Foundation::Collections::IPropertySet DecodePropertySetRoot(TLV::TLVReader & reader)
{
    Windows::Foundation::IInspectable decoded;
    CheckChipError(DecodeTlvValue(reader, decoded), L"Decode Matter TLV");
    if (auto properties = decoded.try_as<Windows::Foundation::Collections::IPropertySet>())
    {
        return properties;
    }
    Windows::Foundation::Collections::PropertySet result;
    result.Insert(L"value", decoded);
    return result;
}

template <typename Value>
Value PropertySetScalar(Windows::Foundation::Collections::IPropertySet const & values, wchar_t const * description)
{
    if (!values || !values.HasKey(L"value") || !values.Lookup(L"value"))
    {
        throw hresult_error(E_BOUNDS, hstring(description) + L" is null or missing.");
    }
    try
    {
        if constexpr (std::is_integral_v<Value> && std::is_unsigned_v<Value> && !std::is_same_v<Value, bool>)
        {
            uint64_t decoded = unbox_value<uint64_t>(values.Lookup(L"value"));
            if (decoded > std::numeric_limits<Value>::max())
            {
                throw hresult_error(E_BOUNDS, hstring(description) + L" is outside the projected numeric range.");
            }
            return static_cast<Value>(decoded);
        }
        else if constexpr (std::is_integral_v<Value> && std::is_signed_v<Value>)
        {
            int64_t decoded = unbox_value<int64_t>(values.Lookup(L"value"));
            if (decoded < std::numeric_limits<Value>::min() || decoded > std::numeric_limits<Value>::max())
            {
                throw hresult_error(E_BOUNDS, hstring(description) + L" is outside the projected numeric range.");
            }
            return static_cast<Value>(decoded);
        }
        else
        {
            return unbox_value<Value>(values.Lookup(L"value"));
        }
    }
    catch (hresult_error const &)
    {
        throw hresult_error(E_INVALIDARG, hstring(description) + L" has an unexpected Matter type.");
    }
}

struct PairingState
{
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    Optional<chip::Controller::CommissioningStage> activeStage;
    Optional<chip::Controller::CommissioningStage> lastCompletedStage;
    CompletionStatus completionStatus;
    bool hasCompletionStatus = false;
    CommissioningProgressCallback progressCallback;
};

void AppendStage(std::wstring & message, wchar_t const * label, Optional<chip::Controller::CommissioningStage> stage)
{
    if (!stage.HasValue())
    {
        return;
    }

    chip::Controller::CommissioningStage value = stage.Value();
    char const * name                           = StageToString(value);
    message.append(L" ");
    message.append(label);
    message.append(L": ");
    if (name != nullptr && name[0] != '\0')
    {
        message.append(to_hstring(name).c_str());
        message.append(L" (");
        message.append(std::to_wstring(static_cast<uint16_t>(value)));
        message.append(L")");
    }
    else
    {
        message.append(std::to_wstring(static_cast<uint16_t>(value)));
    }
    message.append(L".");
}

std::wstring CommissioningTimeoutMessage(PairingState const & state)
{
    std::wstring message = L"Matter commissioning timed out.";
    AppendStage(message, L"Active stage", state.activeStage);
    AppendStage(message, L"Last completed stage", state.lastCompletedStage);
    return message;
}

[[noreturn]] void ThrowCommissioningError(PairingState const & state)
{
    std::wstring message = L"Commission device: ";
    message.append(to_hstring(state.error.AsString()).c_str());
    message.append(L".");
    AppendStage(message, L"Failed stage", state.completionStatus.failedStage);
    AppendStage(message, L"Active stage", state.activeStage);
    AppendStage(message, L"Last completed stage", state.lastCompletedStage);

    if (state.completionStatus.commissioningError.HasValue())
    {
        message.append(L" Commissioning error: ");
        message.append(std::to_wstring(static_cast<uint8_t>(state.completionStatus.commissioningError.Value())));
        message.append(L".");
    }
    if (state.completionStatus.networkCommissioningStatus.HasValue())
    {
        message.append(L" Network commissioning status: ");
        message.append(std::to_wstring(static_cast<uint8_t>(state.completionStatus.networkCommissioningStatus.Value())));
        message.append(L".");
    }
    if (state.completionStatus.connectNetworkErrorValue.HasValue())
    {
        message.append(L" Connect network error: ");
        message.append(std::to_wstring(state.completionStatus.connectNetworkErrorValue.Value()));
        message.append(L".");
    }
    if (state.completionStatus.operationalCertStatus.HasValue())
    {
        message.append(L" Operational certificate status: ");
        message.append(std::to_wstring(static_cast<uint8_t>(state.completionStatus.operationalCertStatus.Value())));
        message.append(L".");
    }
    if (state.completionStatus.attestationResult.HasValue())
    {
        message.append(L" Attestation result: ");
        message.append(std::to_wstring(static_cast<uint16_t>(state.completionStatus.attestationResult.Value())));
        message.append(L".");
    }
    throw hresult_error(HRESULT_FROM_WIN32(ERROR_GEN_FAILURE), message);
}

class PairingDelegate final : public DevicePairingDelegate, public Credentials::DeviceAttestationDelegate
{
public:
    PairingDelegate(PairingState & state, bool allowTestAttestation) : mState(state), mAllowTestAttestation(allowTestAttestation) {}

    void Reset(CommissioningProgressCallback progressCallback = {})
    {
        std::scoped_lock lock(mState.mutex);
        mState.complete               = false;
        mState.error                  = CHIP_NO_ERROR;
        mState.activeStage            = NullOptional;
        mState.lastCompletedStage     = NullOptional;
        mState.completionStatus       = CompletionStatus();
        mState.hasCompletionStatus    = false;
        mState.progressCallback       = std::move(progressCallback);
    }

    void OnCommissioningComplete(NodeId, CHIP_ERROR error) override
    {
        Complete(error);
    }

    void OnCommissioningFailure(PeerId, const CompletionStatus & completionStatus) override
    {
        {
            std::scoped_lock lock(mState.mutex);
            mState.completionStatus    = completionStatus;
            mState.hasCompletionStatus = true;
            mState.error               = completionStatus.err;
            mState.complete            = true;
        }
        mState.condition.notify_all();
    }

    void OnCommissioningStageStart(PeerId, chip::Controller::CommissioningStage stageStarting) override
    {
        CommissioningProgressCallback callback;
        {
            std::scoped_lock lock(mState.mutex);
            mState.activeStage = MakeOptional(stageStarting);
            callback           = mState.progressCallback;
        }
        if (callback)
        {
            callback({ stageStarting, CHIP_NO_ERROR, false, false });
        }
    }

    void OnCommissioningStatusUpdate(PeerId, chip::Controller::CommissioningStage stageCompleted, CHIP_ERROR error) override
    {
        CommissioningProgressCallback callback;
        {
            std::scoped_lock lock(mState.mutex);
            mState.lastCompletedStage = MakeOptional(stageCompleted);
            mState.activeStage        = NullOptional;
            callback                  = mState.progressCallback;
        }
        if (callback)
        {
            callback({ stageCompleted, error, true, false });
        }
    }

    void OnCommissioningRetry(PeerId, chip::Controller::CommissioningStage stage, CHIP_ERROR error) override
    {
        CommissioningProgressCallback callback;
        {
            std::scoped_lock lock(mState.mutex);
            callback = mState.progressCallback;
        }
        if (callback)
        {
            callback({ stage, error, false, true });
        }
    }

    Optional<uint16_t> FailSafeExpiryTimeoutSecs() const override { return NullOptional; }

    void OnDeviceAttestationCompleted(DeviceCommissioner * commissioner, DeviceProxy * device,
                                      const Credentials::DeviceAttestationVerifier::AttestationDeviceInfo &,
                                      Credentials::AttestationVerificationResult result) override
    {
        if (result != Credentials::AttestationVerificationResult::kSuccess && !mAllowTestAttestation)
        {
            Complete(CHIP_ERROR_CERT_NOT_TRUSTED);
            return;
        }

        CHIP_ERROR error =
            commissioner->ContinueCommissioningAfterDeviceAttestation(device, Credentials::AttestationVerificationResult::kSuccess);
        if (error != CHIP_NO_ERROR)
        {
            Complete(error);
        }
    }

private:
    void Complete(CHIP_ERROR error)
    {
        {
            std::scoped_lock lock(mState.mutex);
            mState.complete = true;
            mState.error    = error;
        }
        mState.condition.notify_all();
    }

    PairingState & mState;
    bool mAllowTestAttestation;
};

enum class OnOffOperation
{
    Read,
    On,
    Off,
    Toggle,
};

struct OnOffOperationState
{
    OnOffOperationState(OnOffOperation requestedOperation, EndpointId requestedEndpoint) :
        operation(requestedOperation), endpoint(requestedEndpoint), onConnected(&HandleConnected, this),
        onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void Finish(CHIP_ERROR result)
    {
        {
            std::scoped_lock lock(mutex);
            if (complete)
            {
                return;
            }
            error    = result;
            complete = true;
        }
        condition.notify_all();
    }

    void FinishRead(bool value)
    {
        valueRead = value;
        Finish(CHIP_NO_ERROR);
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<OnOffOperationState *>(context);
        CHIP_ERROR error;
        if (state->operation == OnOffOperation::Read)
        {
            ClusterBase cluster(exchangeManager, session, state->endpoint);
            error = cluster.ReadAttribute<app::Clusters::OnOff::Attributes::OnOff::TypeInfo>(
                state, [](void * callbackContext, bool value) { static_cast<OnOffOperationState *>(callbackContext)->FinishRead(value); },
                [](void * callbackContext, CHIP_ERROR readError) {
                    static_cast<OnOffOperationState *>(callbackContext)->Finish(readError);
                });
        }
        else
        {
            auto success = [state](app::ConcreteCommandPath const &, app::StatusIB const & status,
                                   app::DataModel::NullObjectType const &) { state->Finish(status.ToChipError()); };
            auto failure = [state](CHIP_ERROR commandError) { state->Finish(commandError); };
            if (state->operation == OnOffOperation::On)
            {
                app::Clusters::OnOff::Commands::On::Type request;
                error = InvokeCommandRequest(&exchangeManager, session, state->endpoint, request, success, failure);
            }
            else if (state->operation == OnOffOperation::Off)
            {
                app::Clusters::OnOff::Commands::Off::Type request;
                error = InvokeCommandRequest(&exchangeManager, session, state->endpoint, request, success, failure);
            }
            else
            {
                app::Clusters::OnOff::Commands::Toggle::Type request;
                error = InvokeCommandRequest(&exchangeManager, session, state->endpoint, request, success, failure);
            }
        }
        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<OnOffOperationState *>(context)->Finish(error);
    }

    OnOffOperation operation;
    EndpointId endpoint;
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    bool valueRead = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;
};

template <typename Request>
struct CommandOperationState
{
    CommandOperationState(EndpointId requestedEndpoint, Request requested) :
        endpoint(requestedEndpoint), request(std::move(requested)), onConnected(&HandleConnected, this),
        onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void Finish(CHIP_ERROR result)
    {
        {
            std::scoped_lock lock(mutex);
            if (complete)
            {
                return;
            }
            complete = true;
            error    = result;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<CommandOperationState *>(context);
        auto success = [state](app::ConcreteCommandPath const &, app::StatusIB const & status,
                               typename Request::ResponseType const &) { state->Finish(status.ToChipError()); };
        auto failure = [state](CHIP_ERROR error) { state->Finish(error); };
        CHIP_ERROR error = InvokeCommandRequest(&exchangeManager, session, state->endpoint, state->request, success, failure);
        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<CommandOperationState *>(context)->Finish(error);
    }

    EndpointId endpoint;
    Request request;
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;
};

template <typename AttributeType, typename Value>
struct ReadOperationState
{
    using DecodableType = typename AttributeType::DecodableType;
    using DecodableArgType = typename AttributeType::DecodableArgType;
    using Converter = std::function<Value(DecodableArgType)>;

    ReadOperationState(EndpointId requestedEndpoint, Converter valueConverter) :
        endpoint(requestedEndpoint), converter(std::move(valueConverter)), onConnected(&HandleConnected, this),
        onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void Finish(CHIP_ERROR result)
    {
        {
            std::scoped_lock lock(mutex);
            if (complete)
            {
                return;
            }
            complete = true;
            error    = result;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<ReadOperationState *>(context);
        ClusterBase cluster(exchangeManager, session, state->endpoint);
        CHIP_ERROR error = cluster.ReadAttribute<AttributeType>(
            state,
            [](void * callbackContext, DecodableArgType decoded) {
                auto * operation = static_cast<ReadOperationState *>(callbackContext);
                operation->value = operation->converter(decoded);
                operation->Finish(CHIP_NO_ERROR);
            },
            [](void * callbackContext, CHIP_ERROR readError) {
                static_cast<ReadOperationState *>(callbackContext)->Finish(readError);
            });
        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<ReadOperationState *>(context)->Finish(error);
    }

    EndpointId endpoint;
    Converter converter;
    Value value{};
    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    CHIP_ERROR error = CHIP_NO_ERROR;
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;
};

class GenericReadState final : public app::ReadClient::Callback
{
public:
    using ReportCallback =
        std::function<void(app::ConcreteDataAttributePath const &, Windows::Foundation::Collections::IPropertySet const &)>;

    GenericReadState(EndpointId endpoint, ClusterId cluster, AttributeId attribute, bool subscription,
                     uint16_t minimumInterval, uint16_t maximumInterval, ReportCallback reportCallback = {}) :
        mPath(endpoint, cluster, attribute), mSubscription(subscription), mMinimumInterval(minimumInterval),
        mMaximumInterval(maximumInterval), mReportCallback(std::move(reportCallback)),
        onConnected(&HandleConnected, this), onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void OnAttributeData(app::ConcreteDataAttributePath const & path, TLV::TLVReader * data,
                         app::StatusIB const & status) override
    {
        CHIP_ERROR error = status.ToChipError();
        if (error != CHIP_NO_ERROR || data == nullptr)
        {
            Fail(error != CHIP_NO_ERROR ? error : CHIP_ERROR_INVALID_ARGUMENT);
            return;
        }
        try
        {
            auto decoded = DecodePropertySetRoot(*data);
            {
                std::scoped_lock lock(mutex);
                value      = decoded;
                valueReady = true;
            }
            if (mReportCallback)
            {
                mReportCallback(path, decoded);
            }
        }
        catch (hresult_error const &)
        {
            Fail(CHIP_ERROR_DECODE_FAILED);
        }
        condition.notify_all();
    }

    void OnError(CHIP_ERROR error) override { Fail(error); }

    void OnDone(app::ReadClient *) override
    {
        if (!mSubscription)
        {
            std::scoped_lock lock(mutex);
            complete = true;
            condition.notify_all();
        }
    }

    void OnSubscriptionEstablished(SubscriptionId) override
    {
        {
            std::scoped_lock lock(mutex);
            established = true;
        }
        condition.notify_all();
    }

    void Fail(CHIP_ERROR error)
    {
        {
            std::scoped_lock lock(mutex);
            if (closed)
            {
                return;
            }
            result   = error;
            complete = true;
        }
        condition.notify_all();
    }

    void Close()
    {
        PlatformMgr().LockChipStack();
        onConnected.Cancel();
        onConnectionFailure.Cancel();
        client.reset();
        PlatformMgr().UnlockChipStack();
        {
            std::scoped_lock lock(mutex);
            closed   = true;
            complete = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<GenericReadState *>(context);
        state->client = Platform::MakeUnique<app::ReadClient>(
            app::InteractionModelEngine::GetInstance(), &exchangeManager, *state,
            state->mSubscription ? app::ReadClient::InteractionType::Subscribe : app::ReadClient::InteractionType::Read);
        if (!state->client)
        {
            state->Fail(CHIP_ERROR_NO_MEMORY);
            return;
        }
        app::ReadPrepareParams parameters(session);
        parameters.mpAttributePathParamsList     = &state->mPath;
        parameters.mAttributePathParamsListSize  = 1;
        parameters.mIsFabricFiltered             = true;
        parameters.mMinIntervalFloorSeconds      = state->mMinimumInterval;
        parameters.mMaxIntervalCeilingSeconds    = state->mMaximumInterval;
        CHIP_ERROR error = state->client->SendRequest(parameters);
        if (error != CHIP_NO_ERROR)
        {
            state->Fail(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<GenericReadState *>(context)->Fail(error);
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    bool established = false;
    bool valueReady = false;
    bool closed = false;
    CHIP_ERROR result = CHIP_NO_ERROR;
    Windows::Foundation::Collections::IPropertySet value{ nullptr };
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;

private:
    app::AttributePathParams mPath;
    bool mSubscription;
    uint16_t mMinimumInterval;
    uint16_t mMaximumInterval;
    ReportCallback mReportCallback;
    Platform::UniquePtr<app::ReadClient> client;
};

class GenericEventReadState final : public app::ReadClient::Callback
{
public:
    using ReportCallback = std::function<void(Controller::EventValue const &)>;

    GenericEventReadState(EndpointId endpoint, ClusterId cluster, EventId event, bool urgent, EventNumber minimumEventNumber,
                          bool subscription, uint16_t minimumInterval, uint16_t maximumInterval,
                          ReportCallback reportCallback = {}) :
        mPath(endpoint, cluster, event, urgent), mMinimumEventNumber(minimumEventNumber), mSubscription(subscription),
        mMinimumInterval(minimumInterval), mMaximumInterval(maximumInterval), mReportCallback(std::move(reportCallback)),
        onConnected(&HandleConnected, this), onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void OnEventData(app::EventHeader const & header, TLV::TLVReader * data, app::StatusIB const * status) override
    {
        CHIP_ERROR error = status == nullptr ? CHIP_NO_ERROR : status->ToChipError();
        if (error != CHIP_NO_ERROR || data == nullptr)
        {
            Fail(error != CHIP_NO_ERROR ? error : CHIP_ERROR_INVALID_ARGUMENT);
            return;
        }
        try
        {
            auto decoded = DecodePropertySetRoot(*data);
            auto path = winrt::make<implementation::EventPath>(header.mPath.mEndpointId, header.mPath.mClusterId,
                                                               header.mPath.mEventId, mPath.mIsUrgentEvent);
            auto value = winrt::make<implementation::EventValue>(path, header.mEventNumber, decoded);
            if (!mSubscription)
            {
                std::scoped_lock lock(mutex);
                values.push_back(value);
            }
            if (mReportCallback)
            {
                mReportCallback(value);
            }
        }
        catch (hresult_error const &)
        {
            Fail(CHIP_ERROR_DECODE_FAILED);
        }
        condition.notify_all();
    }

    void OnError(CHIP_ERROR error) override { Fail(error); }

    void OnDone(app::ReadClient *) override
    {
        if (!mSubscription)
        {
            std::scoped_lock lock(mutex);
            complete = true;
            condition.notify_all();
        }
    }

    void OnSubscriptionEstablished(SubscriptionId) override
    {
        {
            std::scoped_lock lock(mutex);
            established = true;
        }
        condition.notify_all();
    }

    void Fail(CHIP_ERROR error)
    {
        {
            std::scoped_lock lock(mutex);
            if (closed)
            {
                return;
            }
            result   = error;
            complete = true;
        }
        condition.notify_all();
    }

    void Close()
    {
        PlatformMgr().LockChipStack();
        onConnected.Cancel();
        onConnectionFailure.Cancel();
        client.reset();
        PlatformMgr().UnlockChipStack();
        {
            std::scoped_lock lock(mutex);
            closed   = true;
            complete = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<GenericEventReadState *>(context);
        state->client = Platform::MakeUnique<app::ReadClient>(
            app::InteractionModelEngine::GetInstance(), &exchangeManager, *state,
            state->mSubscription ? app::ReadClient::InteractionType::Subscribe : app::ReadClient::InteractionType::Read);
        if (!state->client)
        {
            state->Fail(CHIP_ERROR_NO_MEMORY);
            return;
        }
        app::ReadPrepareParams parameters(session);
        parameters.mpEventPathParamsList       = &state->mPath;
        parameters.mEventPathParamsListSize    = 1;
        parameters.mEventNumber.SetValue(state->mMinimumEventNumber);
        parameters.mIsFabricFiltered           = true;
        parameters.mMinIntervalFloorSeconds    = state->mMinimumInterval;
        parameters.mMaxIntervalCeilingSeconds  = state->mMaximumInterval;
        CHIP_ERROR error = state->client->SendRequest(parameters);
        if (error != CHIP_NO_ERROR)
        {
            state->Fail(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<GenericEventReadState *>(context)->Fail(error);
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    bool established = false;
    bool closed = false;
    CHIP_ERROR result = CHIP_NO_ERROR;
    std::vector<Controller::EventValue> values;
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;

private:
    app::EventPathParams mPath;
    EventNumber mMinimumEventNumber;
    bool mSubscription;
    uint16_t mMinimumInterval;
    uint16_t mMaximumInterval;
    ReportCallback mReportCallback;
    Platform::UniquePtr<app::ReadClient> client;
};

class GenericWriteState final : public app::WriteClient::Callback
{
public:
    GenericWriteState(EndpointId endpoint, ClusterId cluster, AttributeId attribute, std::vector<uint8_t> encoded,
                      std::optional<uint16_t> timedWriteTimeout) :
        mPath(endpoint, cluster, attribute), mEncoded(std::move(encoded)), mTimedWriteTimeout(timedWriteTimeout),
        onConnected(&HandleConnected, this), onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void OnResponse(app::WriteClient const *, app::ConcreteDataAttributePath const &, app::StatusIB status) override
    {
        CHIP_ERROR error = status.ToChipError();
        if (error != CHIP_NO_ERROR)
        {
            result = error;
        }
    }

    void OnError(app::WriteClient const *, CHIP_ERROR error) override { result = error; }

    void OnDone(app::WriteClient *) override
    {
        {
            std::scoped_lock lock(mutex);
            complete = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<GenericWriteState *>(context);
        Optional<uint16_t> timedWriteTimeout;
        if (state->mTimedWriteTimeout.has_value())
        {
            timedWriteTimeout.SetValue(*state->mTimedWriteTimeout);
        }
        state->client = Platform::MakeUnique<app::WriteClient>(&exchangeManager, state, timedWriteTimeout);
        if (!state->client)
        {
            state->Finish(CHIP_ERROR_NO_MEMORY);
            return;
        }
        TLV::TLVReader reader;
        reader.Init(state->mEncoded.data(), state->mEncoded.size());
        CHIP_ERROR error = reader.Next();
        if (error == CHIP_NO_ERROR)
        {
            error = state->client->PutPreencodedAttribute(state->mPath, reader);
        }
        if (error == CHIP_NO_ERROR)
        {
            error = state->client->SendWriteRequest(session);
        }
        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<GenericWriteState *>(context)->Finish(error);
    }

    void Finish(CHIP_ERROR error)
    {
        {
            std::scoped_lock lock(mutex);
            result   = error;
            complete = true;
        }
        condition.notify_all();
    }

    void ReleaseClient()
    {
        PlatformMgr().LockChipStack();
        onConnected.Cancel();
        onConnectionFailure.Cancel();
        client.reset();
        PlatformMgr().UnlockChipStack();
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    CHIP_ERROR result = CHIP_NO_ERROR;
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;

private:
    app::ConcreteDataAttributePath mPath;
    std::vector<uint8_t> mEncoded;
    std::optional<uint16_t> mTimedWriteTimeout;
    Platform::UniquePtr<app::WriteClient> client;
};

class GenericInvokeState final : public app::CommandSender::Callback
{
public:
    GenericInvokeState(EndpointId endpoint, ClusterId cluster, CommandId command, std::vector<uint8_t> encoded,
                       std::optional<uint16_t> timedInvokeTimeout) :
        mPath(endpoint, cluster, command, app::CommandPathFlags::kEndpointIdValid), mEncoded(std::move(encoded)),
        mTimedInvokeTimeout(timedInvokeTimeout), onConnected(&HandleConnected, this),
        onConnectionFailure(&HandleConnectionFailure, this)
    {}

    void OnResponse(app::CommandSender *, app::ConcreteCommandPath const &, app::StatusIB const & status,
                    TLV::TLVReader * data) override
    {
        CHIP_ERROR error = status.ToChipError();
        if (error != CHIP_NO_ERROR)
        {
            result = error;
            return;
        }
        if (data != nullptr)
        {
            try
            {
                value = DecodePropertySetRoot(*data);
            }
            catch (hresult_error const &)
            {
                result = CHIP_ERROR_DECODE_FAILED;
            }
        }
        else
        {
            value = Windows::Foundation::Collections::PropertySet();
        }
    }

    void OnError(app::CommandSender const *, CHIP_ERROR error) override { result = error; }

    void OnDone(app::CommandSender *) override
    {
        {
            std::scoped_lock lock(mutex);
            complete = true;
        }
        condition.notify_all();
    }

    static void HandleConnected(void * context, Messaging::ExchangeManager & exchangeManager, SessionHandle const & session)
    {
        auto * state = static_cast<GenericInvokeState *>(context);
        state->sender =
            Platform::MakeUnique<app::CommandSender>(state, &exchangeManager, state->mTimedInvokeTimeout.has_value());
        if (!state->sender)
        {
            state->Finish(CHIP_ERROR_NO_MEMORY);
            return;
        }
        app::CommandSender::PrepareCommandParameters prepareParameters;
        CHIP_ERROR error = state->sender->PrepareCommand(state->mPath, prepareParameters);
        TLV::TLVWriter * writer = state->sender->GetCommandDataIBTLVWriter();
        if (error == CHIP_NO_ERROR && writer == nullptr)
        {
            error = CHIP_ERROR_INCORRECT_STATE;
        }
        if (error == CHIP_NO_ERROR)
        {
            TLV::TLVReader reader;
            reader.Init(state->mEncoded.data(), state->mEncoded.size());
            error = reader.Next();
            if (error == CHIP_NO_ERROR)
            {
                error = writer->CopyContainer(TLV::ContextTag(app::CommandDataIB::Tag::kFields), reader);
            }
        }
        if (error == CHIP_NO_ERROR)
        {
            Optional<uint16_t> timedInvokeTimeout;
            if (state->mTimedInvokeTimeout.has_value())
            {
                timedInvokeTimeout.SetValue(*state->mTimedInvokeTimeout);
            }
            app::CommandSender::FinishCommandParameters finishParameters(timedInvokeTimeout);
            error = state->sender->FinishCommand(finishParameters);
        }
        if (error == CHIP_NO_ERROR)
        {
            error = state->sender->SendCommandRequest(session);
        }
        if (error != CHIP_NO_ERROR)
        {
            state->Finish(error);
        }
    }

    static void HandleConnectionFailure(void * context, ScopedNodeId const &, CHIP_ERROR error)
    {
        static_cast<GenericInvokeState *>(context)->Finish(error);
    }

    void Finish(CHIP_ERROR error)
    {
        {
            std::scoped_lock lock(mutex);
            result   = error;
            complete = true;
        }
        condition.notify_all();
    }

    void ReleaseSender()
    {
        PlatformMgr().LockChipStack();
        onConnected.Cancel();
        onConnectionFailure.Cancel();
        sender.reset();
        PlatformMgr().UnlockChipStack();
    }

    std::mutex mutex;
    std::condition_variable condition;
    bool complete = false;
    CHIP_ERROR result = CHIP_NO_ERROR;
    Windows::Foundation::Collections::IPropertySet value{ nullptr };
    chip::Callback::Callback<OnDeviceConnected> onConnected;
    chip::Callback::Callback<OnDeviceConnectionFailure> onConnectionFailure;

private:
    app::CommandPathParams mPath;
    std::vector<uint8_t> mEncoded;
    std::optional<uint16_t> mTimedInvokeTimeout;
    Platform::UniquePtr<app::CommandSender> sender;
};

std::string GenerateSetupCode(uint32_t pinCode, uint16_t discriminator)
{
    SetupPayload payload;
    payload.setUpPINCode = pinCode;
    payload.discriminator.SetLongValue(discriminator);
    std::string setupCode;
    CheckChipError(ManualSetupPayloadGenerator(payload).payloadDecimalStringRepresentation(setupCode),
                   L"Generate manual setup code");
    return setupCode;
}

} // namespace

class ControllerRuntime : public std::enable_shared_from_this<ControllerRuntime>
{
public:
    static std::shared_ptr<ControllerRuntime> Create(Controller::ControllerOptions const & options)
    {
        if (options == nullptr || options.StoragePath().empty())
        {
            throw hresult_invalid_argument(L"ControllerOptions.StoragePath is required.");
        }

        std::scoped_lock lock(sMutex);
        if (!sInstance.expired())
        {
            throw hresult_illegal_method_call(L"Only one MatterController may be active in a process.");
        }

        auto runtime = std::shared_ptr<ControllerRuntime>(new ControllerRuntime(options));
        runtime->Initialize();
        sInstance = runtime;
        return runtime;
    }

    void Close()
    {
        std::scoped_lock operationLock(mOperationMutex);
        std::scoped_lock lock(mMutex);
        if (mClosed)
        {
            return;
        }

        for (auto const & subscription : mSubscriptions)
        {
            subscription->Close();
        }
        mSubscriptions.clear();
        for (auto const & subscription : mEventSubscriptions)
        {
            subscription->Close();
        }
        mEventSubscriptions.clear();
        (void) PlatformMgr().StopEventLoopTask();
        PlatformMgr().LockChipStack();
        if (mCommissionerInitialized)
        {
            mCommissioner.Shutdown();
            mCommissionerInitialized = false;
        }
        if (mFactoryInitialized)
        {
            DeviceControllerFactory::GetInstance().Shutdown();
            mFactoryInitialized = false;
        }
        Credentials::SetGroupDataProvider(nullptr);
        mGroupDataProvider.Finish();
        mOpCertStore.Finish();
        mOperationalKeystore.Finish();
        PlatformMgr().UnlockChipStack();
        Platform::MemoryShutdown();
        mClosed = true;
    }

    uint16_t FabricIndex()
    {
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        return mCommissioner.GetFabricIndex();
    }

    Controller::CommissionedNode Commission(uint64_t nodeId, uint32_t pinCode, uint16_t discriminator, bool useBle,
                                            std::string const & providedSetupCode = {},
                                            std::optional<WiFiCredentials> const & wiFiCredentials = std::nullopt,
                                            ByteSpan threadOperationalDataset = {},
                                            Optional<AddressResolve::InterfaceSelection> interfaceSelection = NullOptional,
                                            CommissioningProgressCallback progressCallback = {},
                                            std::shared_ptr<std::atomic_bool> cancellationRequested = {})
    {
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        mPairingDelegate.Reset(std::move(progressCallback));
        std::string setupCode = providedSetupCode.empty() ? GenerateSetupCode(pinCode, discriminator) : providedSetupCode;

        CommissioningParameters parameters;
        parameters.SetDeviceAttestationDelegate(&mPairingDelegate);
        if (wiFiCredentials.has_value())
        {
            parameters.SetWiFiCredentials(*wiFiCredentials);
        }
        if (!threadOperationalDataset.empty())
        {
            parameters.SetThreadOperationalDataset(threadOperationalDataset);
        }
        if (interfaceSelection.HasValue())
        {
            parameters.SetOperationalInterfaceSelection(interfaceSelection.Value());
        }
        PlatformMgr().LockChipStack();
        CHIP_ERROR error = mCommissioner.PairDevice(
            nodeId, setupCode.c_str(), parameters, useBle ? DiscoveryType::kDiscoveryBleOnly : DiscoveryType::kDiscoveryNetworkOnly);
        PlatformMgr().UnlockChipStack();
        CheckChipError(error, L"Start commissioning");
        if (cancellationRequested && cancellationRequested->load(std::memory_order_acquire))
        {
            CancelCommission(nodeId);
        }

        std::unique_lock lock(mPairingState.mutex);
        if (!mPairingState.condition.wait_for(lock, kCommissioningTimeout, [this]() { return mPairingState.complete; }))
        {
            std::wstring timeoutMessage = CommissioningTimeoutMessage(mPairingState);
            lock.unlock();
            PlatformMgr().LockChipStack();
            (void) mCommissioner.StopPairing(nodeId);
            PlatformMgr().UnlockChipStack();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), timeoutMessage);
        }
        if (mPairingState.error != CHIP_NO_ERROR && !mPairingState.hasCompletionStatus)
        {
            (void) mPairingState.condition.wait_for(lock, kCommissioningFailureDetailTimeout,
                                                    [this]() { return mPairingState.hasCompletionStatus; });
        }
        if (mPairingState.error != CHIP_NO_ERROR && mPairingState.hasCompletionStatus)
        {
            ThrowCommissioningError(mPairingState);
        }
        CheckChipError(mPairingState.error, L"Commission device");
        if (std::find(mCommissionedNodes.begin(), mCommissionedNodes.end(), nodeId) == mCommissionedNodes.end())
        {
            mCommissionedNodes.push_back(nodeId);
            PersistCommissionedNodes();
        }
        return winrt::make<implementation::CommissionedNode>(nodeId, mCommissioner.GetFabricIndex());
    }

    void CancelCommission(uint64_t nodeId)
    {
        PlatformMgr().LockChipStack();
        (void) mCommissioner.StopPairing(nodeId);
        PlatformMgr().UnlockChipStack();
    }

    std::vector<uint64_t> CommissionedNodes()
    {
        std::scoped_lock lock(mOperationMutex);
        EnsureOpen();
        return mCommissionedNodes;
    }

    Controller::CommissionedNode RecoverNode(uint64_t nodeId)
    {
        if (!IsOperationalNodeId(nodeId))
        {
            throw hresult_invalid_argument(L"nodeId must be an operational Matter node identifier.");
        }

        (void) ReadAttribute(nodeId, 0, app::Clusters::BasicInformation::Id,
                             app::Clusters::BasicInformation::Attributes::VendorID::Id);

        std::scoped_lock lock(mOperationMutex);
        EnsureOpen();
        if (std::find(mCommissionedNodes.begin(), mCommissionedNodes.end(), nodeId) == mCommissionedNodes.end())
        {
            mCommissionedNodes.push_back(nodeId);
            PersistCommissionedNodes();
        }
        return winrt::make<implementation::CommissionedNode>(nodeId, mCommissioner.GetFabricIndex());
    }

    void RemoveNode(uint64_t nodeId)
    {
        std::scoped_lock lock(mOperationMutex);
        EnsureOpen();
        PlatformMgr().LockChipStack();
        CHIP_ERROR error = mCommissioner.UnpairDevice(nodeId);
        PlatformMgr().UnlockChipStack();
        CheckChipError(error, L"Start removing Matter node");
        mCommissionedNodes.erase(std::remove(mCommissionedNodes.begin(), mCommissionedNodes.end(), nodeId),
                                 mCommissionedNodes.end());
        PersistCommissionedNodes();
    }

    bool RunOnOff(uint64_t nodeId, uint16_t endpointId, OnOffOperation operation)
    {
        if (operation == OnOffOperation::Read)
        {
            return PropertySetScalar<bool>(
                ReadAttribute(nodeId, endpointId, app::Clusters::OnOff::Id, app::Clusters::OnOff::Attributes::OnOff::Id),
                L"OnOff");
        }

        CommandId commandId;
        switch (operation)
        {
        case OnOffOperation::On:
            commandId = app::Clusters::OnOff::Commands::On::Id;
            break;
        case OnOffOperation::Off:
            commandId = app::Clusters::OnOff::Commands::Off::Id;
            break;
        case OnOffOperation::Toggle:
            commandId = app::Clusters::OnOff::Commands::Toggle::Id;
            break;
        default:
            throw hresult_invalid_argument(L"Unsupported On/Off operation.");
        }
        InvokeCommand(nodeId, endpointId, app::Clusters::OnOff::Id, commandId,
                      Windows::Foundation::Collections::PropertySet());
        return false;
    }

    template <typename Request>
    void Invoke(uint64_t nodeId, uint16_t endpointId, Request request)
    {
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        CommandOperationState<Request> state(endpointId, std::move(request));
        PlatformMgr().LockChipStack();
        CHIP_ERROR error = mCommissioner.GetConnectedDevice(nodeId, &state.onConnected, &state.onConnectionFailure);
        PlatformMgr().UnlockChipStack();
        CheckChipError(error, L"Start CASE session");

        std::unique_lock lock(state.mutex);
        if (!state.condition.wait_for(lock, kInteractionTimeout, [&state]() { return state.complete; }))
        {
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter command timed out.");
        }
        CheckChipError(state.error, L"Invoke Matter command");
    }

    template <typename AttributeType, typename Value>
    Value Read(uint64_t nodeId, uint16_t endpointId,
               typename ReadOperationState<AttributeType, Value>::Converter converter)
    {
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        ReadOperationState<AttributeType, Value> state(endpointId, std::move(converter));
        PlatformMgr().LockChipStack();
        CHIP_ERROR error = mCommissioner.GetConnectedDevice(nodeId, &state.onConnected, &state.onConnectionFailure);
        PlatformMgr().UnlockChipStack();
        CheckChipError(error, L"Start CASE session");

        std::unique_lock lock(state.mutex);
        if (!state.condition.wait_for(lock, kInteractionTimeout, [&state]() { return state.complete; }))
        {
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter attribute read timed out.");
        }
        CheckChipError(state.error, L"Read Matter attribute");
        return std::move(state.value);
    }

    Windows::Foundation::Collections::IPropertySet ReadAttribute(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId,
                                                                  AttributeId attributeId)
    {
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        auto state = std::make_shared<GenericReadState>(endpointId, clusterId, attributeId, false, 0, 0);
        StartConnectedOperation(nodeId, state->onConnected, state->onConnectionFailure);
        std::unique_lock lock(state->mutex);
        if (!state->condition.wait_for(lock, kInteractionTimeout, [&state]() { return state->complete; }))
        {
            lock.unlock();
            state->Close();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter attribute read timed out.");
        }
        CHIP_ERROR result = state->result;
        auto value        = state->value;
        lock.unlock();
        state->Close();
        CheckChipError(result, L"Read Matter attribute");
        if (!value)
        {
            throw hresult_error(E_FAIL, L"The Matter attribute read returned no value.");
        }
        return value;
    }

    void WriteAttribute(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId, AttributeId attributeId,
                        Windows::Foundation::Collections::IPropertySet const & value,
                        std::optional<uint16_t> timedWriteTimeout = std::nullopt)
    {
        std::vector<uint8_t> encoded;
        CheckChipError(EncodePropertySetRoot(value, encoded), L"Encode Matter attribute value");
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        GenericWriteState state(endpointId, clusterId, attributeId, std::move(encoded), timedWriteTimeout);
        StartConnectedOperation(nodeId, state.onConnected, state.onConnectionFailure);
        std::unique_lock lock(state.mutex);
        if (!state.condition.wait_for(lock, InteractionWaitTimeout(timedWriteTimeout), [&state]() { return state.complete; }))
        {
            lock.unlock();
            state.ReleaseClient();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter attribute write timed out.");
        }
        CHIP_ERROR result = state.result;
        lock.unlock();
        state.ReleaseClient();
        CheckChipError(result, L"Write Matter attribute");
    }

    Windows::Foundation::Collections::IPropertySet InvokeCommand(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId,
                                                                  CommandId commandId,
                                                                  Windows::Foundation::Collections::IPropertySet const & arguments,
                                                                  std::optional<uint16_t> timedInvokeTimeout = std::nullopt)
    {
        std::vector<uint8_t> encoded;
        CheckChipError(EncodePropertySetRoot(arguments, encoded), L"Encode Matter command arguments");
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        GenericInvokeState state(endpointId, clusterId, commandId, std::move(encoded), timedInvokeTimeout);
        StartConnectedOperation(nodeId, state.onConnected, state.onConnectionFailure);
        std::unique_lock lock(state.mutex);
        if (!state.condition.wait_for(lock, InteractionWaitTimeout(timedInvokeTimeout), [&state]() { return state.complete; }))
        {
            lock.unlock();
            state.ReleaseSender();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter command timed out.");
        }
        CHIP_ERROR result = state.result;
        auto value        = state.value;
        lock.unlock();
        state.ReleaseSender();
        CheckChipError(result, L"Invoke Matter command");
        return value ? value : Windows::Foundation::Collections::PropertySet();
    }

    Controller::AttributeSubscription SubscribeAttribute(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId,
                                                          AttributeId attributeId, uint16_t minimumInterval,
                                                          uint16_t maximumInterval)
    {
        if (minimumInterval > maximumInterval)
        {
            throw hresult_invalid_argument(L"The minimum subscription interval cannot exceed the maximum interval.");
        }
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        auto projectedWeak = std::make_shared<winrt::weak_ref<Controller::AttributeSubscription>>();
        auto state         = std::make_shared<GenericReadState>(
            endpointId, clusterId, attributeId, true, minimumInterval, maximumInterval,
            [projectedWeak](app::ConcreteDataAttributePath const & path,
                            Windows::Foundation::Collections::IPropertySet const & data) {
                if (auto subscription = projectedWeak->get())
                {
                    auto projectedPath =
                        winrt::make<implementation::AttributePath>(path.mEndpointId, path.mClusterId, path.mAttributeId);
                    auto value = winrt::make<implementation::AttributeValue>(projectedPath, data);
                    auto args  = winrt::make<implementation::AttributeReportEventArgs>(value);
                    winrt::get_self<implementation::AttributeSubscription>(subscription)->Publish(args);
                }
            });
        auto runtimeWeak  = weak_from_this();
        auto subscription = winrt::make<implementation::AttributeSubscription>([runtimeWeak, state]() {
            if (auto runtime = runtimeWeak.lock())
            {
                runtime->CloseAttributeSubscription(state);
            }
            else
            {
                state->Close();
            }
        });
        *projectedWeak     = winrt::make_weak(subscription);
        StartConnectedOperation(nodeId, state->onConnected, state->onConnectionFailure);

        std::unique_lock lock(state->mutex);
        if (!state->condition.wait_for(lock, kInteractionTimeout,
                                       [&state]() { return state->established || state->complete; }))
        {
            lock.unlock();
            state->Close();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter subscription timed out.");
        }
        CHIP_ERROR result = state->result;
        bool established  = state->established;
        lock.unlock();
        if (result != CHIP_NO_ERROR)
        {
            state->Close();
        }
        CheckChipError(result, L"Subscribe to Matter attribute");
        if (!established)
        {
            state->Close();
            throw hresult_error(E_FAIL, L"The Matter subscription ended before it was established.");
        }
        mSubscriptions.push_back(state);
        return subscription;
    }

    std::vector<Controller::EventValue> ReadEvents(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId, EventId eventId,
                                                   bool urgent, EventNumber minimumEventNumber)
    {
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        auto state = std::make_shared<GenericEventReadState>(endpointId, clusterId, eventId, urgent, minimumEventNumber, false, 0, 0);
        StartConnectedOperation(nodeId, state->onConnected, state->onConnectionFailure);
        std::unique_lock lock(state->mutex);
        if (!state->condition.wait_for(lock, kInteractionTimeout, [&state]() { return state->complete; }))
        {
            lock.unlock();
            state->Close();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter event read timed out.");
        }
        CHIP_ERROR result = state->result;
        auto values       = std::move(state->values);
        lock.unlock();
        state->Close();
        CheckChipError(result, L"Read Matter events");
        return values;
    }

    Controller::EventSubscription SubscribeEvent(uint64_t nodeId, EndpointId endpointId, ClusterId clusterId, EventId eventId,
                                                 bool urgent, EventNumber minimumEventNumber, uint16_t minimumInterval,
                                                 uint16_t maximumInterval)
    {
        if (minimumInterval > maximumInterval)
        {
            throw hresult_invalid_argument(L"The minimum subscription interval cannot exceed the maximum interval.");
        }
        std::scoped_lock operationLock(mOperationMutex);
        EnsureOpen();
        auto projectedWeak = std::make_shared<winrt::weak_ref<Controller::EventSubscription>>();
        auto state = std::make_shared<GenericEventReadState>(
            endpointId, clusterId, eventId, urgent, minimumEventNumber, true, minimumInterval, maximumInterval,
            [projectedWeak](Controller::EventValue const & value) {
                if (auto subscription = projectedWeak->get())
                {
                    auto args = winrt::make<implementation::EventReportEventArgs>(value);
                    winrt::get_self<implementation::EventSubscription>(subscription)->Publish(args);
                }
            });
        auto runtimeWeak  = weak_from_this();
        auto subscription = winrt::make<implementation::EventSubscription>([runtimeWeak, state]() {
            if (auto runtime = runtimeWeak.lock())
            {
                runtime->CloseEventSubscription(state);
            }
            else
            {
                state->Close();
            }
        });
        *projectedWeak     = winrt::make_weak(subscription);
        StartConnectedOperation(nodeId, state->onConnected, state->onConnectionFailure);

        std::unique_lock lock(state->mutex);
        if (!state->condition.wait_for(lock, kInteractionTimeout,
                                       [&state]() { return state->established || state->complete; }))
        {
            lock.unlock();
            state->Close();
            throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Matter event subscription timed out.");
        }
        CHIP_ERROR result = state->result;
        bool established  = state->established;
        lock.unlock();
        if (result != CHIP_NO_ERROR)
        {
            state->Close();
        }
        CheckChipError(result, L"Subscribe to Matter event");
        if (!established)
        {
            state->Close();
            throw hresult_error(E_FAIL, L"The Matter event subscription ended before it was established.");
        }
        mEventSubscriptions.push_back(state);
        return subscription;
    }

private:
    void EnsureOpen()
    {
        std::scoped_lock lock(mMutex);
        if (mClosed)
        {
            throw hresult_illegal_method_call(L"The Matter controller is closed.");
        }
    }

    void CloseAttributeSubscription(std::shared_ptr<GenericReadState> const & state)
    {
        std::scoped_lock operationLock(mOperationMutex);
        state->Close();
        mSubscriptions.erase(std::remove(mSubscriptions.begin(), mSubscriptions.end(), state), mSubscriptions.end());
    }

    void CloseEventSubscription(std::shared_ptr<GenericEventReadState> const & state)
    {
        std::scoped_lock operationLock(mOperationMutex);
        state->Close();
        mEventSubscriptions.erase(std::remove(mEventSubscriptions.begin(), mEventSubscriptions.end(), state),
                                  mEventSubscriptions.end());
    }

    explicit ControllerRuntime(Controller::ControllerOptions const & options) :
        mStoragePath(options.StoragePath()), mControllerNodeId(options.ControllerNodeId()),
        mBluetoothAdapterId(options.BluetoothAdapterId()), mAllowTestAttestation(options.AllowTestAttestation()),
        mGroupDataProvider(50, 25), mPairingDelegate(mPairingState, mAllowTestAttestation)
    {}

    void Initialize()
    {
        std::filesystem::path storagePath(mStoragePath.c_str());
        if (!storagePath.is_absolute())
        {
            throw hresult_invalid_argument(L"ControllerOptions.StoragePath must be absolute.");
        }

        std::filesystem::create_directories(storagePath);
        std::string storageUtf8 = to_string(mStoragePath);

        CheckChipError(Platform::MemoryInit(), L"Initialize Matter memory");
        CheckChipError(DeviceLayer::Internal::BLEMgrImpl().ConfigureBle(mBluetoothAdapterId, true), L"Configure Bluetooth LE");
        CheckChipError(ConfigurationManagerImpl::GetDefaultInstance().ConfigureStorageRoot(storageUtf8.c_str()),
                       L"Configure controller storage");
        CheckChipError(PlatformMgr().InitChipStack(), L"Initialize Matter platform");
        CheckChipError(PlatformMgr().StartEventLoopTask(), L"Start Matter event loop");

        PlatformMgr().LockChipStack();
        CHIP_ERROR error = InitializeController();
        PlatformMgr().UnlockChipStack();
        if (error != CHIP_NO_ERROR)
        {
            Close();
            ThrowChipError(error, L"Initialize Matter controller");
        }
    }

    void StartConnectedOperation(uint64_t nodeId, chip::Callback::Callback<OnDeviceConnected> & onConnected,
                                 chip::Callback::Callback<OnDeviceConnectionFailure> & onConnectionFailure)
    {
        PlatformMgr().LockChipStack();
        CHIP_ERROR error = mCommissioner.GetConnectedDevice(nodeId, &onConnected, &onConnectionFailure);
        PlatformMgr().UnlockChipStack();
        CheckChipError(error, L"Start CASE session");
    }

    CHIP_ERROR InitializeController()
    {
        ReturnErrorOnFailure(mStorage.Init(&PersistedStorage::KeyValueStoreMgr()));
        LoadCommissionedNodes();
        ReturnErrorOnFailure(mOperationalKeystore.Init(&mStorage));
        ReturnErrorOnFailure(mOpCertStore.Init(&mStorage));

        mGroupDataProvider.SetStorageDelegate(&mStorage);
        mGroupDataProvider.SetSessionKeystore(&mSessionKeystore);
        ReturnErrorOnFailure(mGroupDataProvider.Init());
        Credentials::SetGroupDataProvider(&mGroupDataProvider);

        FactoryInitParams factoryParameters;
        factoryParameters.fabricIndependentStorage = &mStorage;
        factoryParameters.operationalKeystore      = &mOperationalKeystore;
        factoryParameters.opCertStore              = &mOpCertStore;
        factoryParameters.sessionKeystore          = &mSessionKeystore;
        factoryParameters.groupDataProvider        = &mGroupDataProvider;
        factoryParameters.dataModelProvider        = &mDataModelProvider;
        ReturnErrorOnFailure(DeviceControllerFactory::GetInstance().Init(factoryParameters));
        mFactoryInitialized = true;

#pragma warning(suppress : 4996)
        ReturnErrorOnFailure(mCredentialsIssuer.Initialize(mStorage));
        SetupParams commissionerParameters;
        commissionerParameters.operationalCredentialsDelegate = &mCredentialsIssuer;
        commissionerParameters.controllerVendorId             = VendorId::TestVendor1;
        commissionerParameters.pairingDelegate                 = &mPairingDelegate;
        commissionerParameters.deviceAttestationVerifier =
            Credentials::GetDefaultDACVerifier(Credentials::GetTestAttestationTrustStore(), nullptr);

        FabricTable * fabrics = DeviceControllerFactory::GetInstance().GetSystemState()->Fabrics();
        VerifyOrReturnError(fabrics != nullptr, CHIP_ERROR_INCORRECT_STATE);
        bool restored = false;
        if (fabrics->FabricCount() > 0)
        {
            chip::FabricIndex fabricIndex = fabrics->begin()->GetFabricIndex();
            if (fabrics->HasOperationalKeyForFabric(fabricIndex))
            {
                commissionerParameters.fabricIndex.SetValue(fabricIndex);
                restored = true;
            }
            else
            {
                ReturnErrorOnFailure(fabrics->Delete(fabricIndex));
            }
        }

        if (!restored)
        {
            uint8_t csrBuffer[Crypto::kMIN_CSR_Buffer_Size];
            uint8_t nocBuffer[kMaxCHIPDERCertLength];
            uint8_t icacBuffer[kMaxCHIPDERCertLength];
            uint8_t rcacBuffer[kMaxCHIPDERCertLength];
            MutableByteSpan csr(csrBuffer);
            MutableByteSpan noc(nocBuffer);
            MutableByteSpan icac(icacBuffer);
            MutableByteSpan rcac(rcacBuffer);
            Crypto::P256PublicKey operationalPublicKey;

            ReturnErrorOnFailure(fabrics->AllocatePendingOperationalKey(NullOptional, csr));
            ReturnErrorOnFailure(Crypto::VerifyCertificateSigningRequest(csr.data(), csr.size(), operationalPublicKey));
            ReturnErrorOnFailure(mCredentialsIssuer.GenerateNOCChainAfterValidation(
                mControllerNodeId, kControllerFabricId, kUndefinedCATs, operationalPublicKey, rcac, icac, noc));
            commissionerParameters.controllerRCAC = rcac;
            commissionerParameters.controllerICAC = icac;
            commissionerParameters.controllerNOC  = noc;
        }

        ReturnErrorOnFailure(DeviceControllerFactory::GetInstance().SetupCommissioner(commissionerParameters, mCommissioner));
        mCommissionerInitialized = true;

        uint8_t compressedFabricId[sizeof(uint64_t)];
        MutableByteSpan compressedFabricIdSpan(compressedFabricId);
        ReturnErrorOnFailure(mCommissioner.GetCompressedFabricIdBytes(compressedFabricIdSpan));
        return Credentials::SetSingleIpkEpochKey(&mGroupDataProvider, mCommissioner.GetFabricIndex(),
                                                 GroupTesting::DefaultIpkValue::GetDefaultIpk(), compressedFabricIdSpan);
    }

    void LoadCommissionedNodes()
    {
        constexpr size_t kMaximumNodeCount = UINT16_MAX / sizeof(uint64_t);
        std::vector<uint64_t> nodes(1);
        while (true)
        {
            uint16_t size = static_cast<uint16_t>(nodes.size() * sizeof(uint64_t));
            CHIP_ERROR error = mStorage.SyncGetKeyValue(kCommissionedNodesKey, nodes.data(), size);
            if (error == CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)
            {
                return;
            }
            if (error == CHIP_ERROR_BUFFER_TOO_SMALL)
            {
                if (nodes.size() == kMaximumNodeCount)
                {
                    throw hresult_error(E_BOUNDS, L"The commissioned node index is too large.");
                }
                nodes.resize(std::min(nodes.size() * 2, kMaximumNodeCount));
                continue;
            }
            CheckChipError(error, L"Read commissioned node index");
            if (size == 0 || size % sizeof(uint64_t) != 0)
            {
                throw hresult_error(E_UNEXPECTED, L"The commissioned node index is invalid.");
            }
            nodes.resize(size / sizeof(uint64_t));
            mCommissionedNodes = std::move(nodes);
            return;
        }
    }

    void PersistCommissionedNodes()
    {
        if (mCommissionedNodes.empty())
        {
            CHIP_ERROR error = mStorage.SyncDeleteKeyValue(kCommissionedNodesKey);
            if (error != CHIP_ERROR_PERSISTED_STORAGE_VALUE_NOT_FOUND)
            {
                CheckChipError(error, L"Delete commissioned node index");
            }
            return;
        }
        size_t byteCount = mCommissionedNodes.size() * sizeof(uint64_t);
        if (byteCount > UINT16_MAX)
        {
            throw hresult_error(E_BOUNDS, L"Too many commissioned nodes are stored.");
        }
        CheckChipError(mStorage.SyncSetKeyValue(kCommissionedNodesKey, mCommissionedNodes.data(), static_cast<uint16_t>(byteCount)),
                       L"Write commissioned node index");
    }

    static std::mutex sMutex;
    static std::weak_ptr<ControllerRuntime> sInstance;

    std::mutex mMutex;
    std::mutex mOperationMutex;
    hstring mStoragePath;
    uint64_t mControllerNodeId;
    uint32_t mBluetoothAdapterId;
    bool mAllowTestAttestation;
    bool mClosed = false;
    KvsPersistentStorageDelegate mStorage;
    PersistentStorageOperationalKeystore mOperationalKeystore;
    Credentials::PersistentStorageOpCertStore mOpCertStore;
    Crypto::RawKeySessionKeystore mSessionKeystore;
    Credentials::GroupDataProviderImpl mGroupDataProvider;
    ExampleOperationalCredentialsIssuer mCredentialsIssuer;
    Testing::EmptyProvider mDataModelProvider;
    DeviceCommissioner mCommissioner;
    PairingState mPairingState;
    PairingDelegate mPairingDelegate;
    bool mFactoryInitialized = false;
    bool mCommissionerInitialized = false;
    std::vector<uint64_t> mCommissionedNodes;
    std::vector<std::shared_ptr<GenericReadState>> mSubscriptions;
    std::vector<std::shared_ptr<GenericEventReadState>> mEventSubscriptions;
};

std::mutex ControllerRuntime::sMutex;
std::weak_ptr<ControllerRuntime> ControllerRuntime::sInstance;

hstring ControllerOptions::StoragePath() const
{
    return mStoragePath;
}

void ControllerOptions::StoragePath(hstring const & value)
{
    mStoragePath = value;
}

uint64_t ControllerOptions::ControllerNodeId() const
{
    return mControllerNodeId;
}

void ControllerOptions::ControllerNodeId(uint64_t value)
{
    mControllerNodeId = value;
}

uint32_t ControllerOptions::BluetoothAdapterId() const
{
    return mBluetoothAdapterId;
}

void ControllerOptions::BluetoothAdapterId(uint32_t value)
{
    mBluetoothAdapterId = value;
}

bool ControllerOptions::AllowTestAttestation() const
{
    return mAllowTestAttestation;
}

void ControllerOptions::AllowTestAttestation(bool value)
{
    mAllowTestAttestation = value;
}

uint64_t OnNetworkCommissioningParameters::NodeId() const
{
    return mNodeId;
}

void OnNetworkCommissioningParameters::NodeId(uint64_t value)
{
    mNodeId = value;
}

hstring OnNetworkCommissioningParameters::SetupCode() const
{
    return mSetupCode;
}

void OnNetworkCommissioningParameters::SetupCode(hstring const & value)
{
    mSetupCode = value;
}

uint32_t OnNetworkCommissioningParameters::SetupPinCode() const
{
    return mSetupPinCode;
}

void OnNetworkCommissioningParameters::SetupPinCode(uint32_t value)
{
    mSetupPinCode = value;
}

uint16_t OnNetworkCommissioningParameters::LongDiscriminator() const
{
    return mLongDiscriminator;
}

void OnNetworkCommissioningParameters::LongDiscriminator(uint16_t value)
{
    mLongDiscriminator = value;
}

hstring OnNetworkCommissioningParameters::IpAddress() const
{
    return mIpAddress;
}

void OnNetworkCommissioningParameters::IpAddress(hstring const & value)
{
    mIpAddress = value;
}

uint64_t BleCommissioningParameters::NodeId() const
{
    return mNodeId;
}

void BleCommissioningParameters::NodeId(uint64_t value)
{
    mNodeId = value;
}

uint32_t BleCommissioningParameters::SetupPinCode() const
{
    return mSetupPinCode;
}

void BleCommissioningParameters::SetupPinCode(uint32_t value)
{
    mSetupPinCode = value;
}

uint16_t BleCommissioningParameters::LongDiscriminator() const
{
    return mLongDiscriminator;
}

void BleCommissioningParameters::LongDiscriminator(uint16_t value)
{
    mLongDiscriminator = value;
}

WiFiNetworkCredentials::WiFiNetworkCredentials(hstring const & ssid, hstring const & passphrase)
{
    std::vector<uint8_t> encodedSsid       = Utf8Bytes(ssid);
    std::vector<uint8_t> encodedPassphrase = Utf8Bytes(passphrase);
    if (encodedSsid.empty() || encodedSsid.size() > CommissioningParameters::kMaxSsidLen ||
        encodedPassphrase.size() > CommissioningParameters::kMaxCredentialsLen)
    {
        Crypto::ClearSecretData(encodedSsid.data(), encodedSsid.size());
        Crypto::ClearSecretData(encodedPassphrase.data(), encodedPassphrase.size());
        throw hresult_invalid_argument(
            L"ssid must contain 1 to 32 UTF-8 bytes and passphrase must not exceed 64 UTF-8 bytes.");
    }
    mSsid       = std::move(encodedSsid);
    mPassphrase = std::move(encodedPassphrase);
}

WiFiNetworkCredentials::~WiFiNetworkCredentials()
{
    if (!mSsid.empty())
    {
        Crypto::ClearSecretData(mSsid.data(), mSsid.size());
    }
    if (!mPassphrase.empty())
    {
        Crypto::ClearSecretData(mPassphrase.data(), mPassphrase.size());
    }
}

uint32_t WiFiNetworkCredentials::SsidLength() const
{
    return static_cast<uint32_t>(mSsid.size());
}

uint32_t WiFiNetworkCredentials::PassphraseLength() const
{
    return static_cast<uint32_t>(mPassphrase.size());
}

std::vector<uint8_t> const & WiFiNetworkCredentials::Ssid() const
{
    return mSsid;
}

std::vector<uint8_t> const & WiFiNetworkCredentials::Passphrase() const
{
    return mPassphrase;
}

ThreadNetworkCredentials::ThreadNetworkCredentials(Windows::Storage::Streams::IBuffer const & operationalDataset)
{
    if (!operationalDataset)
    {
        throw hresult_invalid_argument(L"operationalDataset cannot be null.");
    }
    if (operationalDataset.Length() == 0 ||
        operationalDataset.Length() > CommissioningParameters::kMaxThreadDatasetLen)
    {
        throw hresult_invalid_argument(L"operationalDataset must contain between 1 and 254 bytes.");
    }
    std::vector<uint8_t> dataset(operationalDataset.Length());
    try
    {
        Windows::Storage::Streams::DataReader::FromBuffer(operationalDataset).ReadBytes(dataset);
    }
    catch (...)
    {
        Crypto::ClearSecretData(dataset.data(), dataset.size());
        throw;
    }
    mOperationalDataset = std::move(dataset);
}

ThreadNetworkCredentials::~ThreadNetworkCredentials()
{
    if (!mOperationalDataset.empty())
    {
        Crypto::ClearSecretData(mOperationalDataset.data(), mOperationalDataset.size());
    }
}

uint32_t ThreadNetworkCredentials::DatasetLength() const
{
    return static_cast<uint32_t>(mOperationalDataset.size());
}

std::vector<uint8_t> const & ThreadNetworkCredentials::OperationalDataset() const
{
    return mOperationalDataset;
}

uint64_t BleNetworkCommissioningParameters::NodeId() const
{
    return mNodeId;
}

void BleNetworkCommissioningParameters::NodeId(uint64_t value)
{
    mNodeId = value;
}

uint32_t BleNetworkCommissioningParameters::SetupPinCode() const
{
    return mSetupPinCode;
}

void BleNetworkCommissioningParameters::SetupPinCode(uint32_t value)
{
    mSetupPinCode = value;
}

uint16_t BleNetworkCommissioningParameters::LongDiscriminator() const
{
    return mLongDiscriminator;
}

void BleNetworkCommissioningParameters::LongDiscriminator(uint16_t value)
{
    mLongDiscriminator = value;
}

Controller::WiFiNetworkCredentials BleNetworkCommissioningParameters::WiFi() const
{
    return mWiFi;
}

void BleNetworkCommissioningParameters::WiFi(Controller::WiFiNetworkCredentials const & value)
{
    mWiFi = value;
}

Controller::ThreadNetworkCredentials BleNetworkCommissioningParameters::Thread() const
{
    return mThread;
}

void BleNetworkCommissioningParameters::Thread(Controller::ThreadNetworkCredentials const & value)
{
    mThread = value;
}

AttributePath::AttributePath(uint16_t endpointId, uint32_t clusterId, uint32_t attributeId) :
    mEndpointId(endpointId), mClusterId(clusterId), mAttributeId(attributeId)
{}

uint16_t AttributePath::EndpointId() const
{
    return mEndpointId;
}

uint32_t AttributePath::ClusterId() const
{
    return mClusterId;
}

uint32_t AttributePath::AttributeId() const
{
    return mAttributeId;
}

CommandPath::CommandPath(uint16_t endpointId, uint32_t clusterId, uint32_t commandId) :
    mEndpointId(endpointId), mClusterId(clusterId), mCommandId(commandId)
{}

uint16_t CommandPath::EndpointId() const
{
    return mEndpointId;
}

uint32_t CommandPath::ClusterId() const
{
    return mClusterId;
}

uint32_t CommandPath::CommandId() const
{
    return mCommandId;
}

EventPath::EventPath(uint16_t endpointId, uint32_t clusterId, uint32_t eventId, bool urgent) :
    mEndpointId(endpointId), mClusterId(clusterId), mEventId(eventId), mUrgent(urgent)
{}

uint16_t EventPath::EndpointId() const
{
    return mEndpointId;
}

uint32_t EventPath::ClusterId() const
{
    return mClusterId;
}

uint32_t EventPath::EventId() const
{
    return mEventId;
}

bool EventPath::Urgent() const
{
    return mUrgent;
}

CommissionedNode::CommissionedNode(uint64_t nodeId, uint16_t fabricIndex) : mNodeId(nodeId), mFabricIndex(fabricIndex) {}

uint64_t CommissionedNode::NodeId() const
{
    return mNodeId;
}

uint16_t CommissionedNode::FabricIndex() const
{
    return mFabricIndex;
}

CommissioningProgressEventArgs::CommissioningProgressEventArgs(Controller::CommissioningStage stage, hstring message) :
    mStage(stage), mMessage(std::move(message))
{}

Controller::CommissioningStage CommissioningProgressEventArgs::Stage() const
{
    return mStage;
}

hstring CommissioningProgressEventArgs::Message() const
{
    return mMessage;
}

MatterNetworkInterfaceSelection::MatterNetworkInterfaceSelection(Controller::MatterNetworkInterfaceSelectionMode mode,
                                                                 uint64_t interfaceId) :
    mMode(mode),
    mInterfaceId(interfaceId)
{
    if ((mode == Controller::MatterNetworkInterfaceSelectionMode::Automatic) != (interfaceId == 0))
    {
        throw hresult_invalid_argument(L"Automatic selection requires interface ID 0; explicit selection requires a nonzero ID.");
    }
}

Controller::MatterNetworkInterfaceSelectionMode MatterNetworkInterfaceSelection::Mode() const
{
    return mMode;
}

uint64_t MatterNetworkInterfaceSelection::InterfaceId() const
{
    return mInterfaceId;
}

MatterNetworkInterface::MatterNetworkInterface(uint64_t id, uint32_t index, hstring name,
                                               Controller::MatterNetworkInterfaceType type, bool connected,
                                               bool supportsIpv6, bool supportsMulticast, bool isVirtual) :
    mId(id),
    mIndex(index), mName(std::move(name)), mType(type), mConnected(connected), mSupportsIpv6(supportsIpv6),
    mSupportsMulticast(supportsMulticast), mIsVirtual(isVirtual)
{}

uint64_t MatterNetworkInterface::Id() const { return mId; }
uint32_t MatterNetworkInterface::InterfaceIndex() const { return mIndex; }
hstring MatterNetworkInterface::Name() const { return mName; }
Controller::MatterNetworkInterfaceType MatterNetworkInterface::Type() const { return mType; }
bool MatterNetworkInterface::IsConnected() const { return mConnected; }
bool MatterNetworkInterface::SupportsIpv6() const { return mSupportsIpv6; }
bool MatterNetworkInterface::SupportsMulticast() const { return mSupportsMulticast; }
bool MatterNetworkInterface::IsVirtual() const { return mIsVirtual; }

MatterCommissioningProgress::MatterCommissioningProgress(
    Controller::MatterCommissioningStage stage, Controller::MatterCommissioningStage lastCompletedStage, int32_t nativeStageId,
    Controller::MatterCommissioningTransport transport, Windows::Foundation::TimeSpan elapsedTime, hstring diagnosticMessage,
    hstring displayMessage, uint64_t networkInterfaceId, hstring networkInterfaceName, uint32_t attemptNumber, bool isRetrying) :
    mStage(stage),
    mLastCompletedStage(lastCompletedStage), mNativeStageId(nativeStageId), mTransport(transport), mElapsedTime(elapsedTime),
    mDiagnosticMessage(std::move(diagnosticMessage)), mDisplayMessage(std::move(displayMessage)),
    mNetworkInterfaceId(networkInterfaceId), mNetworkInterfaceName(std::move(networkInterfaceName)),
    mAttemptNumber(attemptNumber), mIsRetrying(isRetrying)
{}

Controller::MatterCommissioningStage MatterCommissioningProgress::Stage() const { return mStage; }
Controller::MatterCommissioningStage MatterCommissioningProgress::LastCompletedStage() const { return mLastCompletedStage; }
int32_t MatterCommissioningProgress::NativeStageId() const { return mNativeStageId; }
Controller::MatterCommissioningTransport MatterCommissioningProgress::Transport() const { return mTransport; }
Windows::Foundation::TimeSpan MatterCommissioningProgress::ElapsedTime() const { return mElapsedTime; }
hstring MatterCommissioningProgress::DiagnosticMessage() const { return mDiagnosticMessage; }
hstring MatterCommissioningProgress::DisplayMessage() const { return mDisplayMessage; }
uint64_t MatterCommissioningProgress::NetworkInterfaceId() const { return mNetworkInterfaceId; }
hstring MatterCommissioningProgress::NetworkInterfaceName() const { return mNetworkInterfaceName; }
uint32_t MatterCommissioningProgress::AttemptNumber() const { return mAttemptNumber; }
bool MatterCommissioningProgress::IsRetrying() const { return mIsRetrying; }

MatterCommissioningResult::MatterCommissioningResult(
    bool succeeded, Controller::MatterCommissioningOutcome outcome, Controller::MatterCommissioningFailureKind failureKind,
    Controller::MatterCommissioningStage failedStage, Controller::MatterCommissioningStage lastCompletedStage,
    int32_t nativeStageId, int32_t nativeErrorCode, hstring diagnosticMessage, uint64_t networkInterfaceId,
    Controller::CommissionedNode node) :
    mSucceeded(succeeded),
    mOutcome(outcome), mFailureKind(failureKind), mFailedStage(failedStage), mLastCompletedStage(lastCompletedStage),
    mNativeStageId(nativeStageId), mNativeErrorCode(nativeErrorCode), mDiagnosticMessage(std::move(diagnosticMessage)),
    mNetworkInterfaceId(networkInterfaceId), mNode(std::move(node))
{}

bool MatterCommissioningResult::Succeeded() const { return mSucceeded; }
Controller::MatterCommissioningOutcome MatterCommissioningResult::Outcome() const { return mOutcome; }
Controller::MatterCommissioningFailureKind MatterCommissioningResult::FailureKind() const { return mFailureKind; }
Controller::MatterCommissioningStage MatterCommissioningResult::FailedStage() const { return mFailedStage; }
Controller::MatterCommissioningStage MatterCommissioningResult::LastCompletedStage() const { return mLastCompletedStage; }
int32_t MatterCommissioningResult::NativeStageId() const { return mNativeStageId; }
int32_t MatterCommissioningResult::NativeErrorCode() const { return mNativeErrorCode; }
hstring MatterCommissioningResult::DiagnosticMessage() const { return mDiagnosticMessage; }
uint64_t MatterCommissioningResult::NetworkInterfaceId() const { return mNetworkInterfaceId; }
Controller::CommissionedNode MatterCommissioningResult::Node() const { return mNode; }

AttributeValue::AttributeValue(Controller::AttributePath path, Windows::Foundation::Collections::IPropertySet data) :
    mPath(std::move(path)), mData(std::move(data))
{}

Controller::AttributePath AttributeValue::Path() const
{
    return mPath;
}

Windows::Foundation::Collections::IPropertySet AttributeValue::Data() const
{
    return mData;
}

CommandResult::CommandResult(Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet data) :
    mPath(std::move(path)), mData(std::move(data))
{}

Controller::CommandPath CommandResult::Path() const
{
    return mPath;
}

Windows::Foundation::Collections::IPropertySet CommandResult::Data() const
{
    return mData;
}

TimedInteractionOptions::TimedInteractionOptions(uint16_t timeoutMilliseconds) :
    mTimeoutMilliseconds(timeoutMilliseconds)
{
    if (timeoutMilliseconds == 0)
    {
        throw hresult_invalid_argument(L"The timed interaction timeout must be greater than zero.");
    }
}

uint16_t TimedInteractionOptions::TimeoutMilliseconds() const
{
    return mTimeoutMilliseconds;
}

EventValue::EventValue(Controller::EventPath path, uint64_t eventNumber,
                       Windows::Foundation::Collections::IPropertySet data) :
    mPath(std::move(path)), mEventNumber(eventNumber), mData(std::move(data))
{}

Controller::EventPath EventValue::Path() const
{
    return mPath;
}

uint64_t EventValue::EventNumber() const
{
    return mEventNumber;
}

Windows::Foundation::Collections::IPropertySet EventValue::Data() const
{
    return mData;
}

EventReportEventArgs::EventReportEventArgs(Controller::EventValue value) : mValue(std::move(value)) {}

Controller::EventValue EventReportEventArgs::Value() const
{
    return mValue;
}

EventSubscription::EventSubscription(std::function<void()> close) : mClose(std::move(close)) {}

event_token EventSubscription::ReportReceived(
    Windows::Foundation::TypedEventHandler<Controller::EventSubscription, Controller::EventReportEventArgs> const & handler)
{
    std::vector<Controller::EventReportEventArgs> pendingReports;
    std::exception_ptr handlerError;
    event_token token;
    {
        std::scoped_lock lock(mReportMutex);
        token = mReportReceived.add(handler);
        ++mReportHandlerCount;
        if (mReportHandlerCount > 1)
        {
            return token;
        }
        mDrainingPendingReports = true;
        pendingReports.swap(mPendingReports);
    }
    while (true)
    {
        for (auto const & report : pendingReports)
        {
            try
            {
                mReportReceived(*this, report);
            }
            catch (...)
            {
                if (!handlerError)
                {
                    handlerError = std::current_exception();
                }
            }
        }
        std::scoped_lock lock(mReportMutex);
        if (mPendingReports.empty())
        {
            mDrainingPendingReports = false;
            break;
        }
        pendingReports.clear();
        pendingReports.swap(mPendingReports);
    }
    if (handlerError)
    {
        std::scoped_lock lock(mReportMutex);
        mReportReceived.remove(token);
        --mReportHandlerCount;
        std::rethrow_exception(handlerError);
    }
    return token;
}

void EventSubscription::ReportReceived(event_token const & token) noexcept
{
    std::scoped_lock lock(mReportMutex);
    mReportReceived.remove(token);
    if (mReportHandlerCount > 0)
    {
        --mReportHandlerCount;
    }
}

Windows::Foundation::IAsyncAction EventSubscription::CloseAsync()
{
    co_await resume_background();
    std::function<void()> close;
    {
        std::scoped_lock lock(mReportMutex);
        close = std::move(mClose);
    }
    if (close)
    {
        close();
    }
}

void EventSubscription::Publish(Controller::EventReportEventArgs const & args)
{
    {
        std::scoped_lock lock(mReportMutex);
        if (mReportHandlerCount == 0 || mDrainingPendingReports)
        {
            if (mPendingReports.size() == kMaximumPendingReports)
            {
                mPendingReports.erase(mPendingReports.begin());
            }
            mPendingReports.push_back(args);
            return;
        }
    }
    mReportReceived(*this, args);
}

AttributeReportEventArgs::AttributeReportEventArgs(Controller::AttributeValue value) : mValue(std::move(value)) {}

Controller::AttributeValue AttributeReportEventArgs::Value() const
{
    return mValue;
}

AttributeSubscription::AttributeSubscription(std::function<void()> close) : mClose(std::move(close)) {}

event_token AttributeSubscription::ReportReceived(
    Windows::Foundation::TypedEventHandler<Controller::AttributeSubscription, Controller::AttributeReportEventArgs> const & handler)
{
    std::vector<Controller::AttributeReportEventArgs> pendingReports;
    std::exception_ptr handlerError;
    event_token token;
    {
        std::scoped_lock lock(mReportMutex);
        token = mReportReceived.add(handler);
        ++mReportHandlerCount;
        if (mReportHandlerCount > 1)
        {
            return token;
        }
        mDrainingPendingReports = true;
        pendingReports.swap(mPendingReports);
    }
    while (true)
    {
        for (auto const & report : pendingReports)
        {
            try
            {
                mReportReceived(*this, report);
            }
            catch (...)
            {
                if (!handlerError)
                {
                    handlerError = std::current_exception();
                }
            }
        }
        std::scoped_lock lock(mReportMutex);
        if (mPendingReports.empty())
        {
            mDrainingPendingReports = false;
            break;
        }
        pendingReports.clear();
        pendingReports.swap(mPendingReports);
    }
    if (handlerError)
    {
        std::scoped_lock lock(mReportMutex);
        mReportReceived.remove(token);
        --mReportHandlerCount;
        std::rethrow_exception(handlerError);
    }
    return token;
}

void AttributeSubscription::ReportReceived(event_token const & token) noexcept
{
    std::scoped_lock lock(mReportMutex);
    mReportReceived.remove(token);
    if (mReportHandlerCount > 0)
    {
        --mReportHandlerCount;
    }
}

Windows::Foundation::IAsyncAction AttributeSubscription::CloseAsync()
{
    co_await resume_background();
    std::function<void()> close;
    {
        std::scoped_lock lock(mReportMutex);
        close = std::move(mClose);
    }
    if (close)
    {
        close();
    }
}

void AttributeSubscription::Publish(Controller::AttributeReportEventArgs const & args)
{
    {
        std::scoped_lock lock(mReportMutex);
        if (mReportHandlerCount == 0 || mDrainingPendingReports)
        {
            if (mPendingReports.size() == kMaximumPendingReports)
            {
                mPendingReports.erase(mPendingReports.begin());
            }
            mPendingReports.push_back(args);
            return;
        }
    }
    mReportReceived(*this, args);
}

BasicInformation::BasicInformation(uint16_t vendorId, hstring vendorName, uint16_t productId, hstring productName,
                                   hstring nodeLabel, hstring serialNumber, uint32_t softwareVersion,
                                   hstring softwareVersionString) :
    mVendorId(vendorId), mVendorName(std::move(vendorName)), mProductId(productId), mProductName(std::move(productName)),
    mNodeLabel(std::move(nodeLabel)), mSerialNumber(std::move(serialNumber)), mSoftwareVersion(softwareVersion),
    mSoftwareVersionString(std::move(softwareVersionString))
{}

uint16_t BasicInformation::VendorId() const
{
    return mVendorId;
}

hstring BasicInformation::VendorName() const
{
    return mVendorName;
}

uint16_t BasicInformation::ProductId() const
{
    return mProductId;
}

hstring BasicInformation::ProductName() const
{
    return mProductName;
}

hstring BasicInformation::NodeLabel() const
{
    return mNodeLabel;
}

hstring BasicInformation::SerialNumber() const
{
    return mSerialNumber;
}

uint32_t BasicInformation::SoftwareVersion() const
{
    return mSoftwareVersion;
}

hstring BasicInformation::SoftwareVersionString() const
{
    return mSoftwareVersionString;
}

OnOffCluster::OnOffCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId) :
    mRuntime(std::move(runtime)), mNodeId(nodeId), mEndpointId(endpointId)
{}

Windows::Foundation::IAsyncOperation<bool> OnOffCluster::ReadAsync()
{
    co_await resume_background();
    co_return mRuntime->RunOnOff(mNodeId, mEndpointId, OnOffOperation::Read);
}

Windows::Foundation::IAsyncAction OnOffCluster::SetAsync(bool value)
{
    co_await resume_background();
    mRuntime->RunOnOff(mNodeId, mEndpointId, value ? OnOffOperation::On : OnOffOperation::Off);
}

Windows::Foundation::IAsyncAction OnOffCluster::OnAsync()
{
    co_await resume_background();
    mRuntime->RunOnOff(mNodeId, mEndpointId, OnOffOperation::On);
}

Windows::Foundation::IAsyncAction OnOffCluster::OffAsync()
{
    co_await resume_background();
    mRuntime->RunOnOff(mNodeId, mEndpointId, OnOffOperation::Off);
}

Windows::Foundation::IAsyncAction OnOffCluster::ToggleAsync()
{
    co_await resume_background();
    mRuntime->RunOnOff(mNodeId, mEndpointId, OnOffOperation::Toggle);
}

Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription>
OnOffCluster::SubscribeAsync(uint16_t minimumIntervalSeconds, uint16_t maximumIntervalSeconds)
{
    co_await resume_background();
    co_return mRuntime->SubscribeAttribute(mNodeId, mEndpointId, app::Clusters::OnOff::Id,
                                           app::Clusters::OnOff::Attributes::OnOff::Id, minimumIntervalSeconds,
                                           maximumIntervalSeconds);
}

LevelControlCluster::LevelControlCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId) :
    mRuntime(std::move(runtime)), mNodeId(nodeId), mEndpointId(endpointId)
{}

Windows::Foundation::IAsyncOperation<uint8_t> LevelControlCluster::ReadCurrentLevelAsync()
{
    co_await resume_background();
    co_return PropertySetScalar<uint8_t>(
        mRuntime->ReadAttribute(mNodeId, mEndpointId, app::Clusters::LevelControl::Id,
                                app::Clusters::LevelControl::Attributes::CurrentLevel::Id),
        L"CurrentLevel");
}

Windows::Foundation::IAsyncAction LevelControlCluster::MoveToLevelAsync(uint8_t level, uint16_t transitionTime,
                                                                       uint8_t optionsMask, uint8_t optionsOverride)
{
    co_await resume_background();
    Windows::Foundation::Collections::PropertySet arguments;
    arguments.Insert(L"0", box_value(level));
    arguments.Insert(L"1", box_value(transitionTime));
    arguments.Insert(L"2", box_value(optionsMask));
    arguments.Insert(L"3", box_value(optionsOverride));
    mRuntime->InvokeCommand(mNodeId, mEndpointId, app::Clusters::LevelControl::Id,
                            app::Clusters::LevelControl::Commands::MoveToLevel::Id, arguments);
}

Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription>
LevelControlCluster::SubscribeAsync(uint16_t minimumIntervalSeconds, uint16_t maximumIntervalSeconds)
{
    co_await resume_background();
    co_return mRuntime->SubscribeAttribute(mNodeId, mEndpointId, app::Clusters::LevelControl::Id,
                                           app::Clusters::LevelControl::Attributes::CurrentLevel::Id, minimumIntervalSeconds,
                                           maximumIntervalSeconds);
}

BasicInformationCluster::BasicInformationCluster(std::shared_ptr<ControllerRuntime> runtime, uint64_t nodeId, uint16_t endpointId) :
    mRuntime(std::move(runtime)), mNodeId(nodeId), mEndpointId(endpointId)
{}

Windows::Foundation::IAsyncOperation<Controller::BasicInformation> BasicInformationCluster::ReadAsync()
{
    co_await resume_background();
    using namespace app::Clusters::BasicInformation::Attributes;
    auto readValue = [this](AttributeId attributeId) {
        return mRuntime->ReadAttribute(mNodeId, mEndpointId, app::Clusters::BasicInformation::Id, attributeId);
    };
    uint16_t vendorId  = PropertySetScalar<uint16_t>(readValue(VendorID::Id), L"VendorID");
    hstring vendorName = PropertySetScalar<hstring>(readValue(VendorName::Id), L"VendorName");
    uint16_t productId = PropertySetScalar<uint16_t>(readValue(ProductID::Id), L"ProductID");
    hstring productName = PropertySetScalar<hstring>(readValue(ProductName::Id), L"ProductName");
    hstring nodeLabel   = PropertySetScalar<hstring>(readValue(NodeLabel::Id), L"NodeLabel");
    hstring serialNumber;
    try
    {
        serialNumber = PropertySetScalar<hstring>(readValue(SerialNumber::Id), L"SerialNumber");
    }
    catch (hresult_error const & error)
    {
        if (to_string(error.message()).find("UNSUPPORTED_ATTRIBUTE") == std::string::npos)
        {
            throw;
        }
    }
    uint32_t softwareVersion = PropertySetScalar<uint32_t>(readValue(SoftwareVersion::Id), L"SoftwareVersion");
    hstring softwareVersionString =
        PropertySetScalar<hstring>(readValue(SoftwareVersionString::Id), L"SoftwareVersionString");
    co_return winrt::make<implementation::BasicInformation>(vendorId, vendorName, productId, productName, nodeLabel, serialNumber,
                                                            softwareVersion, softwareVersionString);
}

MatterController::MatterController(std::shared_ptr<ControllerRuntime> runtime) : mRuntime(std::move(runtime)) {}

std::shared_ptr<ControllerRuntime> MatterController::Runtime()
{
    std::scoped_lock lock(mRuntimeMutex);
    if (!mRuntime)
    {
        throw hresult_illegal_method_call(L"The Matter controller is closed.");
    }
    return mRuntime;
}

Windows::Foundation::IAsyncOperation<Controller::MatterController>
MatterController::CreateAsync(Controller::ControllerOptions options)
{
    co_await resume_background();
    co_return winrt::make<MatterController>(ControllerRuntime::Create(options));
}

event_token MatterController::CommissioningProgress(
    Windows::Foundation::TypedEventHandler<Controller::MatterController, Controller::CommissioningProgressEventArgs> const & handler)
{
    return mCommissioningProgress.add(handler);
}

void MatterController::CommissioningProgress(event_token const & token) noexcept
{
    mCommissioningProgress.remove(token);
}

Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
MatterController::CommissionOnNetworkAsync(Controller::OnNetworkCommissioningParameters parameters)
{
    if (!parameters)
    {
        throw hresult_invalid_argument(L"parameters cannot be null.");
    }
    auto progress = winrt::make<implementation::CommissioningProgressEventArgs>(Controller::CommissioningStage::Discovering,
                                                                                L"Starting on-network commissioning");
    mCommissioningProgress(*this, progress);
    auto runtime = Runtime();
    co_await resume_background();
    auto node = runtime->Commission(parameters.NodeId(), parameters.SetupPinCode(), parameters.LongDiscriminator(), false,
                                    to_string(parameters.SetupCode()));
    progress = winrt::make<implementation::CommissioningProgressEventArgs>(Controller::CommissioningStage::Complete,
                                                                           L"Commissioning complete");
    mCommissioningProgress(*this, progress);
    co_return node;
}

Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
MatterController::CommissionBleAsync(Controller::BleCommissioningParameters parameters)
{
    if (!parameters)
    {
        throw hresult_invalid_argument(L"parameters cannot be null.");
    }
    auto progress = winrt::make<implementation::CommissioningProgressEventArgs>(Controller::CommissioningStage::Discovering,
                                                                                L"Starting BLE commissioning");
    mCommissioningProgress(*this, progress);
    auto runtime = Runtime();
    co_await resume_background();
    auto node = runtime->Commission(parameters.NodeId(), parameters.SetupPinCode(), parameters.LongDiscriminator(), true);
    progress = winrt::make<implementation::CommissioningProgressEventArgs>(Controller::CommissioningStage::Complete,
                                                                           L"Commissioning complete");
    mCommissioningProgress(*this, progress);
    co_return node;
}

Windows::Foundation::Collections::IVectorView<Controller::CommissionedNode> MatterController::CommissionedNodes()
{
    auto runtime = Runtime();
    std::vector<Controller::CommissionedNode> nodes;
    for (uint64_t nodeId : runtime->CommissionedNodes())
    {
        nodes.push_back(winrt::make<implementation::CommissionedNode>(nodeId, runtime->FabricIndex()));
    }
    return single_threaded_vector(std::move(nodes)).GetView();
}

Windows::Foundation::IAsyncAction MatterController::RemoveNodeAsync(uint64_t nodeId)
{
    auto runtime = Runtime();
    co_await resume_background();
    runtime->RemoveNode(nodeId);
}

Windows::Foundation::IAsyncOperation<Controller::AttributeValue>
MatterController::ReadAttributeAsync(uint64_t nodeId, Controller::AttributePath path)
{
    if (!path)
    {
        throw hresult_invalid_argument(L"path cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    auto data = runtime->ReadAttribute(nodeId, path.EndpointId(), path.ClusterId(), path.AttributeId());
    co_return winrt::make<implementation::AttributeValue>(path, data);
}

Windows::Foundation::IAsyncAction MatterController::WriteAttributeAsync(
    uint64_t nodeId, Controller::AttributePath path, Windows::Foundation::Collections::IPropertySet value)
{
    if (!path || !value)
    {
        throw hresult_invalid_argument(L"path and value cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    runtime->WriteAttribute(nodeId, path.EndpointId(), path.ClusterId(), path.AttributeId(), value);
}

Windows::Foundation::IAsyncAction MatterController::WriteAttributeTimedAsync(
    uint64_t nodeId, Controller::AttributePath path, Windows::Foundation::Collections::IPropertySet value,
    Controller::TimedInteractionOptions options)
{
    if (!path || !value || !options)
    {
        throw hresult_invalid_argument(L"path, value, and options cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    runtime->WriteAttribute(nodeId, path.EndpointId(), path.ClusterId(), path.AttributeId(), value,
                            options.TimeoutMilliseconds());
}

Windows::Foundation::IAsyncOperation<Controller::CommandResult> MatterController::InvokeCommandAsync(
    uint64_t nodeId, Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet arguments)
{
    if (!path || !arguments)
    {
        throw hresult_invalid_argument(L"path and arguments cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    auto data = runtime->InvokeCommand(nodeId, path.EndpointId(), path.ClusterId(), path.CommandId(), arguments);
    co_return winrt::make<implementation::CommandResult>(path, data);
}

Windows::Foundation::IAsyncOperation<Controller::CommandResult> MatterController::InvokeCommandTimedAsync(
    uint64_t nodeId, Controller::CommandPath path, Windows::Foundation::Collections::IPropertySet arguments,
    Controller::TimedInteractionOptions options)
{
    if (!path || !arguments || !options)
    {
        throw hresult_invalid_argument(L"path, arguments, and options cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    auto data = runtime->InvokeCommand(nodeId, path.EndpointId(), path.ClusterId(), path.CommandId(), arguments,
                                       options.TimeoutMilliseconds());
    co_return winrt::make<implementation::CommandResult>(path, data);
}

Windows::Foundation::IAsyncOperation<Controller::AttributeSubscription>
MatterController::SubscribeAttributeAsync(uint64_t nodeId, Controller::AttributePath path, uint16_t minimumIntervalSeconds,
                                          uint16_t maximumIntervalSeconds)
{
    if (!path)
    {
        throw hresult_invalid_argument(L"path cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    co_return runtime->SubscribeAttribute(nodeId, path.EndpointId(), path.ClusterId(), path.AttributeId(),
                                          minimumIntervalSeconds, maximumIntervalSeconds);
}

Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::EventValue>>
MatterController::ReadEventsAsync(uint64_t nodeId, Controller::EventPath path, uint64_t minimumEventNumber)
{
    if (!path)
    {
        throw hresult_invalid_argument(L"path cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    auto values = runtime->ReadEvents(nodeId, path.EndpointId(), path.ClusterId(), path.EventId(), path.Urgent(),
                                      minimumEventNumber);
    co_return single_threaded_vector(std::move(values)).GetView();
}

Windows::Foundation::IAsyncOperation<Controller::EventSubscription>
MatterController::SubscribeEventAsync(uint64_t nodeId, Controller::EventPath path, uint64_t minimumEventNumber,
                                      uint16_t minimumIntervalSeconds, uint16_t maximumIntervalSeconds)
{
    if (!path)
    {
        throw hresult_invalid_argument(L"path cannot be null.");
    }
    auto runtime = Runtime();
    co_await resume_background();
    co_return runtime->SubscribeEvent(nodeId, path.EndpointId(), path.ClusterId(), path.EventId(), path.Urgent(),
                                      minimumEventNumber, minimumIntervalSeconds, maximumIntervalSeconds);
}

Controller::OnOffCluster MatterController::GetOnOffCluster(uint64_t nodeId, uint16_t endpointId)
{
    return winrt::make<OnOffCluster>(Runtime(), nodeId, endpointId);
}

Controller::LevelControlCluster MatterController::GetLevelControlCluster(uint64_t nodeId, uint16_t endpointId)
{
    return winrt::make<LevelControlCluster>(Runtime(), nodeId, endpointId);
}

Controller::BasicInformationCluster MatterController::GetBasicInformationCluster(uint64_t nodeId, uint16_t endpointId)
{
    return winrt::make<BasicInformationCluster>(Runtime(), nodeId, endpointId);
}

Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
MatterControllerRecovery::RecoverNodeAsync(uint64_t nodeId)
{
    co_await resume_background();
    co_return mRuntime->RecoverNode(nodeId);
}

MatterControllerRecovery::MatterControllerRecovery(Controller::MatterController controller)
{
    if (!controller)
    {
        throw hresult_invalid_argument(L"controller cannot be null.");
    }
    mRuntime = get_self<implementation::MatterController>(controller)->Runtime();
}

MatterControllerNetworkCommissioning::MatterControllerNetworkCommissioning(Controller::MatterController controller)
{
    if (!controller)
    {
        throw hresult_invalid_argument(L"controller cannot be null.");
    }
    mRuntime = get_self<implementation::MatterController>(controller)->Runtime();
}

Windows::Foundation::IAsyncOperation<Controller::CommissionedNode>
MatterControllerNetworkCommissioning::CommissionBleAsync(Controller::BleNetworkCommissioningParameters parameters)
{
    if (!parameters)
    {
        throw hresult_invalid_argument(L"parameters cannot be null.");
    }

    auto wiFi  = parameters.WiFi();
    auto thread = parameters.Thread();
    if (static_cast<bool>(wiFi) == static_cast<bool>(thread))
    {
        throw hresult_invalid_argument(L"Provide exactly one Wi-Fi or Thread credential set.");
    }

    std::optional<WiFiCredentials> nativeWiFi;
    ByteSpan threadDataset;
    if (wiFi)
    {
        auto implementation = get_self<implementation::WiFiNetworkCredentials>(wiFi);
        auto const & ssid       = implementation->Ssid();
        auto const & passphrase = implementation->Passphrase();
        nativeWiFi.emplace(ByteSpan(ssid.data(), ssid.size()), ByteSpan(passphrase.data(), passphrase.size()));
    }
    else
    {
        auto const & dataset = get_self<implementation::ThreadNetworkCredentials>(thread)->OperationalDataset();
        threadDataset        = ByteSpan(dataset.data(), dataset.size());
    }

    uint64_t nodeId        = parameters.NodeId();
    uint32_t setupPinCode  = parameters.SetupPinCode();
    uint16_t discriminator = parameters.LongDiscriminator();
    co_await resume_background();
    co_return mRuntime->Commission(nodeId, setupPinCode, discriminator, true, {}, nativeWiFi, threadDataset);
}

Windows::Foundation::IAsyncOperation<Windows::Foundation::Collections::IVectorView<Controller::MatterNetworkInterface>>
MatterNetworkInterfaceProvider::GetEligibleNetworkInterfacesAsync()
{
    co_await resume_background();
    std::vector<Controller::MatterNetworkInterface> interfaces;
    for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
    {
        Inet::InterfaceId id = iterator.GetInterfaceId();
        if (!id.IsPresent() || iterator.IsLoopback())
        {
            continue;
        }

        char nameBuffer[Inet::InterfaceId::kMaxIfNameLength] = {};
        if (iterator.GetInterfaceName(nameBuffer, sizeof(nameBuffer)) != CHIP_NO_ERROR)
        {
            continue;
        }

        Inet::InterfaceType nativeType = Inet::InterfaceType::Unknown;
        (void) iterator.GetInterfaceType(nativeType);
        bool supportsIpv6 = false;
        for (Inet::InterfaceAddressIterator addressIterator; addressIterator.HasCurrent(); addressIterator.Next())
        {
            Inet::IPAddress address;
            if (addressIterator.GetInterfaceId() == id && addressIterator.GetAddress(address) == CHIP_NO_ERROR && address.IsIPv6())
            {
                supportsIpv6 = true;
                break;
            }
        }

        std::string name = nameBuffer;
        std::string lowerName(name);
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
                       [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        bool isVirtual = lowerName.find("virtual") != std::string::npos || lowerName.find("vethernet") != std::string::npos ||
            lowerName.find("hyper-v") != std::string::npos || lowerName.find("vpn") != std::string::npos;
        interfaces.push_back(winrt::make<implementation::MatterNetworkInterface>(
            id.GetPlatformInterface(), id.GetInterfaceIndex(), to_hstring(name),
            static_cast<Controller::MatterNetworkInterfaceType>(nativeType), iterator.IsUp(), supportsIpv6,
            iterator.SupportsMulticast(), isVirtual));
    }
    co_return single_threaded_vector(std::move(interfaces)).GetView();
}

MatterControllerCommissioning::MatterControllerCommissioning(Controller::MatterController controller)
{
    if (!controller)
    {
        throw hresult_invalid_argument(L"controller cannot be null.");
    }
    mRuntime = get_self<implementation::MatterController>(controller)->Runtime();
}

event_token MatterControllerCommissioning::ProgressChanged(
    Windows::Foundation::TypedEventHandler<Controller::MatterControllerCommissioning,
                                           Controller::MatterCommissioningProgress> const & handler)
{
    return mProgressChanged.add(handler);
}

void MatterControllerCommissioning::ProgressChanged(event_token const & token) noexcept
{
    mProgressChanged.remove(token);
}

void MatterControllerCommissioning::Publish(Controller::MatterCommissioningProgress const & progress)
{
    bool scheduleDrain = false;
    {
        std::scoped_lock lock(mProgressMutex);
        mPendingProgress.push_back(progress);
        if (!mProgressDrainScheduled)
        {
            mProgressDrainScheduled = true;
            scheduleDrain            = true;
        }
    }
    if (scheduleDrain)
    {
        auto strong = get_strong();
        Windows::System::Threading::ThreadPool::RunAsync(
            [strong = std::move(strong)](Windows::Foundation::IAsyncAction const &) { strong->DrainProgress(); });
    }
}

void MatterControllerCommissioning::DrainProgress()
{
    while (true)
    {
        Controller::MatterCommissioningProgress progress{ nullptr };
        {
            std::scoped_lock lock(mProgressMutex);
            if (mPendingProgress.empty())
            {
                mProgressDrainScheduled = false;
                mProgressCondition.notify_all();
                return;
            }
            progress = mPendingProgress.front();
            mPendingProgress.erase(mPendingProgress.begin());
        }
        try
        {
            mProgressChanged(*this, progress);
        }
        catch (...)
        {
        }
    }
}

void MatterControllerCommissioning::WaitForProgressDrain()
{
    std::unique_lock lock(mProgressMutex);
    mProgressCondition.wait(lock, [this]() { return !mProgressDrainScheduled && mPendingProgress.empty(); });
}

Windows::Foundation::IAsyncOperation<Controller::MatterCommissioningResult>
MatterControllerCommissioning::CommissionBleAsync(Controller::BleNetworkCommissioningParameters parameters,
                                                   Controller::MatterNetworkInterfaceSelection interfaceSelection)
{
    auto lifetime = get_strong();
    if (!parameters)
    {
        throw hresult_invalid_argument(L"parameters cannot be null.");
    }
    if (!interfaceSelection)
    {
        interfaceSelection = winrt::make<implementation::MatterNetworkInterfaceSelection>(
            Controller::MatterNetworkInterfaceSelectionMode::Automatic, 0);
    }

    auto wiFi   = parameters.WiFi();
    auto thread = parameters.Thread();
    if (static_cast<bool>(wiFi) == static_cast<bool>(thread))
    {
        throw hresult_invalid_argument(L"Provide exactly one Wi-Fi or Thread credential set.");
    }

    std::optional<WiFiCredentials> nativeWiFi;
    ByteSpan threadDataset;
    if (wiFi)
    {
        auto implementation     = get_self<implementation::WiFiNetworkCredentials>(wiFi);
        auto const & ssid       = implementation->Ssid();
        auto const & passphrase = implementation->Passphrase();
        nativeWiFi.emplace(ByteSpan(ssid.data(), ssid.size()), ByteSpan(passphrase.data(), passphrase.size()));
    }
    else
    {
        auto const & dataset = get_self<implementation::ThreadNetworkCredentials>(thread)->OperationalDataset();
        threadDataset        = ByteSpan(dataset.data(), dataset.size());
    }

    AddressResolve::InterfaceSelection nativeSelection;
    nativeSelection.interfaceId = Inet::InterfaceId(interfaceSelection.InterfaceId());
    switch (interfaceSelection.Mode())
    {
    case Controller::MatterNetworkInterfaceSelectionMode::Automatic:
        nativeSelection.mode = AddressResolve::InterfaceSelectionMode::kAutomatic;
        break;
    case Controller::MatterNetworkInterfaceSelectionMode::PreferSpecifiedInterface:
        nativeSelection.mode = AddressResolve::InterfaceSelectionMode::kPrefer;
        break;
    case Controller::MatterNetworkInterfaceSelectionMode::RequireSpecifiedInterface:
        nativeSelection.mode = AddressResolve::InterfaceSelectionMode::kRequire;
        break;
    default:
        throw hresult_invalid_argument(L"Unknown network interface selection mode.");
    }

    hstring interfaceName;
    if (nativeSelection.mode != AddressResolve::InterfaceSelectionMode::kAutomatic)
    {
        bool found             = false;
        bool connected         = false;
        bool supportsMulticast = false;
        bool supportsIpv6      = false;
        for (Inet::InterfaceIterator iterator; iterator.HasCurrent(); iterator.Next())
        {
            if (iterator.GetInterfaceId() != nativeSelection.interfaceId)
            {
                continue;
            }
            found             = true;
            connected         = iterator.IsUp();
            supportsMulticast = iterator.SupportsMulticast();
            char nameBuffer[Inet::InterfaceId::kMaxIfNameLength] = {};
            if (iterator.GetInterfaceName(nameBuffer, sizeof(nameBuffer)) == CHIP_NO_ERROR)
            {
                interfaceName = to_hstring(nameBuffer);
            }
            break;
        }
        if (found)
        {
            for (Inet::InterfaceAddressIterator iterator; iterator.HasCurrent(); iterator.Next())
            {
                Inet::IPAddress address;
                if (iterator.GetInterfaceId() == nativeSelection.interfaceId &&
                    iterator.GetAddress(address) == CHIP_NO_ERROR && address.IsIPv6())
                {
                    supportsIpv6 = true;
                    break;
                }
            }
        }
        if (nativeSelection.mode == AddressResolve::InterfaceSelectionMode::kRequire)
        {
            if (!found)
            {
                throw hresult_invalid_argument(L"PreferredInterfaceNotFound");
            }
            if (!connected)
            {
                throw hresult_invalid_argument(L"PreferredInterfaceDisconnected");
            }
            if (!supportsIpv6)
            {
                throw hresult_invalid_argument(L"PreferredInterfaceDoesNotSupportIpv6");
            }
            if (!supportsMulticast)
            {
                throw hresult_invalid_argument(L"PreferredInterfaceDoesNotSupportMulticast");
            }
        }
    }

    struct ProgressState
    {
        std::mutex mutex;
        Controller::MatterCommissioningStage lastCompleted = Controller::MatterCommissioningStage::Unknown;
        Controller::MatterCommissioningStage active        = Controller::MatterCommissioningStage::SearchingForDevice;
        int32_t nativeStageId                               = -1;
        int32_t nativeError                                 = 0;
        uint32_t attemptNumber                              = 1;
        uint64_t networkInterfaceId                         = 0;
        hstring networkInterfaceName;
    };

    auto state = std::make_shared<ProgressState>();
    auto start = std::chrono::steady_clock::now();
    uint64_t interfaceId = interfaceSelection.InterfaceId();
    state->networkInterfaceId   = interfaceId;
    state->networkInterfaceName = interfaceName;
    auto selectionMode          = interfaceSelection.Mode();
    auto weak            = get_weak();
    Publish(winrt::make<implementation::MatterCommissioningProgress>(
        Controller::MatterCommissioningStage::SearchingForDevice, Controller::MatterCommissioningStage::Unknown, -1,
        Controller::MatterCommissioningTransport::Bluetooth, Windows::Foundation::TimeSpan::zero(),
        L"Searching for the Matter device over Bluetooth.", L"Searching for device", interfaceId, interfaceName, 1, false));

    CommissioningProgressCallback callback = [weak, state, start, selectionMode](const NativeCommissioningProgress & update) {
        auto self = weak.get();
        if (!self)
        {
            return;
        }
        Controller::MatterCommissioningStage stage = ProjectCommissioningStage(update.stage);
        Controller::MatterCommissioningStage lastCompleted;
        uint32_t attemptNumber;
        uint64_t currentInterfaceId;
        hstring currentInterfaceName;
        {
            std::scoped_lock lock(state->mutex);
            state->active        = stage;
            state->nativeStageId = static_cast<int32_t>(update.stage);
            state->nativeError   = update.error.AsInteger();
            if (update.completed && update.error == CHIP_NO_ERROR)
            {
                state->lastCompleted = stage;
            }
            if (update.retrying)
            {
                ++state->attemptNumber;
                if (selectionMode == Controller::MatterNetworkInterfaceSelectionMode::PreferSpecifiedInterface)
                {
                    state->networkInterfaceId   = 0;
                    state->networkInterfaceName = {};
                }
            }
            lastCompleted        = state->lastCompleted;
            attemptNumber        = state->attemptNumber;
            currentInterfaceId   = state->networkInterfaceId;
            currentInterfaceName = state->networkInterfaceName;
        }
        char const * nativeName = StageToString(update.stage);
        hstring message = to_hstring(nativeName != nullptr && nativeName[0] != '\0' ? nativeName : "Unknown commissioning stage");
        auto elapsed = std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::steady_clock::now() - start);
        self->Publish(winrt::make<implementation::MatterCommissioningProgress>(
            stage, lastCompleted, static_cast<int32_t>(update.stage), ProjectCommissioningTransport(update.stage), elapsed,
            message, message, currentInterfaceId, currentInterfaceName, attemptNumber, update.retrying));
    };

    uint64_t nodeId        = parameters.NodeId();
    uint32_t setupPinCode  = parameters.SetupPinCode();
    uint16_t discriminator = parameters.LongDiscriminator();
    auto cancellationRequested = std::make_shared<std::atomic_bool>(false);
    auto cancellation          = co_await get_cancellation_token();
    cancellation.enable_propagation();
    auto runtime = lifetime->mRuntime;
    cancellation.callback([runtime, nodeId, cancellationRequested]() noexcept {
        cancellationRequested->store(true, std::memory_order_release);
        runtime->CancelCommission(nodeId);
    });
    co_await resume_background();
    if (cancellation())
    {
        Publish(winrt::make<implementation::MatterCommissioningProgress>(
            Controller::MatterCommissioningStage::Failed, Controller::MatterCommissioningStage::Unknown, -1,
            Controller::MatterCommissioningTransport::Bluetooth,
            std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::steady_clock::now() - start),
            L"Commissioning was canceled.", L"Commissioning canceled", interfaceId, interfaceName, 1, false));
        WaitForProgressDrain();
        throw hresult_canceled();
    }
    try
    {
        auto node = mRuntime->Commission(nodeId, setupPinCode, discriminator, true, {}, nativeWiFi, threadDataset,
                                         MakeOptional(nativeSelection), std::move(callback), cancellationRequested);
        Controller::MatterCommissioningStage lastCompleted;
        uint32_t attemptNumber;
        uint64_t currentInterfaceId;
        hstring currentInterfaceName;
        {
            std::scoped_lock lock(state->mutex);
            lastCompleted        = state->lastCompleted;
            attemptNumber        = state->attemptNumber;
            currentInterfaceId   = state->networkInterfaceId;
            currentInterfaceName = state->networkInterfaceName;
        }
        Publish(winrt::make<implementation::MatterCommissioningProgress>(
            Controller::MatterCommissioningStage::Completed, lastCompleted, -1,
            Controller::MatterCommissioningTransport::OperationalIp,
            std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::steady_clock::now() - start),
            L"Commissioning completed.", L"Commissioning complete", currentInterfaceId, currentInterfaceName, attemptNumber, false));
        auto result = winrt::make<implementation::MatterCommissioningResult>(
            true, Controller::MatterCommissioningOutcome::Succeeded, Controller::MatterCommissioningFailureKind::Unknown,
            Controller::MatterCommissioningStage::Unknown, lastCompleted, -1, 0, L"Commissioning completed.", interfaceId, node);
        WaitForProgressDrain();
        co_return result;
    }
    catch (hresult_error const & error)
    {
        if (cancellationRequested->load(std::memory_order_acquire))
        {
            Controller::MatterCommissioningStage lastCompleted;
            int32_t nativeStageId;
            uint32_t attemptNumber;
            uint64_t currentInterfaceId;
            hstring currentInterfaceName;
            {
                std::scoped_lock lock(state->mutex);
                lastCompleted        = state->lastCompleted;
                nativeStageId        = state->nativeStageId;
                attemptNumber        = state->attemptNumber;
                currentInterfaceId   = state->networkInterfaceId;
                currentInterfaceName = state->networkInterfaceName;
            }
            Publish(winrt::make<implementation::MatterCommissioningProgress>(
                Controller::MatterCommissioningStage::Failed, lastCompleted, nativeStageId,
                Controller::MatterCommissioningTransport::Unknown,
                std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::steady_clock::now() - start),
                L"Commissioning was canceled.", L"Commissioning canceled", currentInterfaceId, currentInterfaceName,
                attemptNumber, false));
            WaitForProgressDrain();
            throw hresult_canceled();
        }
        Controller::MatterCommissioningStage failedStage;
        Controller::MatterCommissioningStage lastCompleted;
        int32_t nativeStageId;
        int32_t nativeError;
        uint32_t attemptNumber;
        uint64_t currentInterfaceId;
        hstring currentInterfaceName;
        {
            std::scoped_lock lock(state->mutex);
            failedStage         = state->active;
            lastCompleted       = state->lastCompleted;
            nativeStageId       = state->nativeStageId;
            nativeError         = state->nativeError;
            attemptNumber       = state->attemptNumber;
            currentInterfaceId   = state->networkInterfaceId;
            currentInterfaceName = state->networkInterfaceName;
        }
        bool timedOut = error.code() == HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        Publish(winrt::make<implementation::MatterCommissioningProgress>(
            Controller::MatterCommissioningStage::Failed, lastCompleted, nativeStageId,
            Controller::MatterCommissioningTransport::Unknown,
            std::chrono::duration_cast<Windows::Foundation::TimeSpan>(std::chrono::steady_clock::now() - start),
            error.message(), L"Commissioning failed", currentInterfaceId, currentInterfaceName, attemptNumber, false));
        auto result = winrt::make<implementation::MatterCommissioningResult>(
            false, timedOut ? Controller::MatterCommissioningOutcome::TimedOut : Controller::MatterCommissioningOutcome::Failed,
            ClassifyCommissioningFailure(failedStage, timedOut),
            failedStage, lastCompleted, nativeStageId, nativeError, error.message(), interfaceId, nullptr);
        WaitForProgressDrain();
        co_return result;
    }
}

Windows::Foundation::IAsyncAction MatterController::CloseAsync()
{
    std::shared_ptr<ControllerRuntime> runtime;
    {
        std::scoped_lock lock(mRuntimeMutex);
        runtime = std::move(mRuntime);
    }
    if (runtime)
    {
        runtime->Close();
    }
    co_return;
}

} // namespace winrt::Matter::Windows::Controller::implementation
