# Unreal Engine Voice Chat Integration

This document contains the complete Unreal Engine C++ integration for the voice-to-chat system. These files are added to an existing Unreal Engine project to connect it to the whisper transcription server.

## Overview

The integration adds voice chat to any Unreal Engine project with:
- **Push-to-talk** via Ctrl+V
- **TCP connection** to the whisper server on port 9090
- **Stateless locale protocol** — sends language code with every audio message
- **Broadcast support** — receives transcriptions from all connected clients
- **In-game chat widget** — semi-transparent overlay at bottom-left of screen

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  Unreal Engine                                      │
│                                                     │
│  VoiceDemoPlayerController                          │
│    └── VoiceChatComponent                           │
│          ├── AudioCapture (mic input)               │
│          ├── WhisperClient (TCP to server)           │
│          │     ├── DoSendAudio (on Ctrl+V release)  │
│          │     └── ReceiveLoop (background thread)  │
│          └── VoiceChatWidget (UI overlay)            │
└─────────────────────────────────────────────────────┘
         │                          ▲
         │ [locale][audio]          │ [JSON broadcast]
         ▼                          │
┌─────────────────────────────────────────────────────┐
│  Whisper Server (voice-unreal-chatbox)              │
│  - STT via whisper.cpp                              │
│  - Profanity filter                                 │
│  - Broadcast to all clients                         │
└─────────────────────────────────────────────────────┘
```

## Wire Protocol

```
Client → Server:  [1-byte locale length][locale string][4-byte BE audio length][raw 16-bit PCM, 16kHz mono]
Server → Client:  [4-byte BE length][JSON response]
```

On connect, the client sends a registration message (locale + zero-length audio) so the server adds it to the broadcast list immediately.

## File Structure

All voice chat files go in `Source/<ProjectName>/VoiceChat/`:

```
Source/<ProjectName>/
├── VoiceChat/
│   ├── WhisperClient.h          # TCP client with background receive loop
│   ├── WhisperClient.cpp
│   ├── VoiceChatComponent.h     # Actor component: mic capture + push-to-talk
│   ├── VoiceChatComponent.cpp
│   ├── VoiceChatWidget.h        # In-game chat overlay widget
│   └── VoiceChatWidget.cpp
├── <ProjectName>PlayerController.h   # Modified to add VoiceChatComponent
└── <ProjectName>PlayerController.cpp
```

## Required Module Dependencies

Add these to your `.Build.cs` file's `PublicDependencyModuleNames`:

```csharp
"Sockets",
"Networking",
"AudioCapture",
"AudioCaptureCore",
"AudioMixer",
"Json",
"JsonUtilities",
"UMG",
"Slate",
"SlateCore"
```

Also add to `PublicIncludePaths`:
```csharp
"<ProjectName>/VoiceChat"
```

## Source Files

### WhisperClient.h

```cpp
#pragma once

#include "CoreMinimal.h"
#include "HAL/Runnable.h"
#include "WhisperClient.generated.h"

class FSocket;

DECLARE_DELEGATE_TwoParams(FOnTranscriptionReceived, const FString& /*Speaker*/, const FString& /*Text*/);
DECLARE_DELEGATE_OneParam(FOnWhisperError, const FString& /*ErrorMessage*/);

/**
 * TCP client that communicates with the Whisper transcription server.
 * Uses the length-prefixed binary protocol:
 *   Client -> Server: [1-byte locale len][locale][4-byte BE audio length][PCM data]
 *   Server -> Client: [4-byte BE length][JSON response]
 *
 * A background receive thread continuously listens for messages from the
 * server (both responses to our audio and broadcasts from other clients).
 */
UCLASS()
class UWhisperClient : public UObject
{
	GENERATED_BODY()

public:
	/** Connect to the whisper server and start the receive thread. Returns true if connection succeeded. */
	bool Connect(const FString& Host = TEXT("127.0.0.1"), int32 Port = 9090);

	/** Disconnect from the server and stop the receive thread. */
	void Disconnect();

	/** Returns true if currently connected to the server. */
	bool IsConnected() const;

	/** Language locale for STT (e.g. "en", "ja"). Sent with every audio message. */
	FString Locale = TEXT("en");

	/**
	 * Send audio data to the server for transcription.
	 * Audio must be raw 16-bit PCM, 16kHz, mono.
	 * Sends on a background thread; result arrives via OnTranscriptionReceived.
	 */
	void SendAudio(const TArray<uint8>& PCMData);

	/** Called on the game thread when a transcription is received (own or broadcast). */
	FOnTranscriptionReceived OnTranscriptionReceived;

	/** Called on the game thread when an error occurs. */
	FOnWhisperError OnError;

	virtual void BeginDestroy() override;

private:
	/** Send raw bytes over the socket. Returns true on success. */
	bool SendAll(const uint8* Data, int32 Length);

	/** Receive exact number of bytes from the socket. Returns true on success. */
	bool RecvAll(uint8* Data, int32 Length);

	/** Background thread: send locale + audio to server. */
	void DoSendAudio(TArray<uint8> PCMData);

	/** Background receive loop: continuously reads messages from the server. */
	void ReceiveLoop();

	/** Parse a JSON response and fire the appropriate delegate on the game thread. */
	void HandleResponse(const FString& JsonString);

	FSocket* Socket = nullptr;
	FCriticalSection SendMutex;
	FThreadSafeBool bRunning{false};
};
```

### WhisperClient.cpp

```cpp
#include "WhisperClient.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "Common/TcpSocketBuilder.h"
#include "IPAddress.h"

bool UWhisperClient::Connect(const FString& Host, int32 Port)
{
	FScopeLock Lock(&SendMutex);

	if (Socket)
	{
		Disconnect();
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		return false;
	}

	Socket = FTcpSocketBuilder(TEXT("WhisperClient"))
		.AsBlocking()
		.Build();

	if (!Socket)
	{
		return false;
	}

	TSharedRef<FInternetAddr> Addr = SocketSubsystem->CreateInternetAddr();
	bool bIsValid = false;
	Addr->SetIp(*Host, bIsValid);
	if (!bIsValid)
	{
		ESocketErrors ResolveError = SocketSubsystem->GetHostByName(TCHAR_TO_ANSI(*Host), *Addr);
		if (ResolveError != SE_NO_ERROR)
		{
			SocketSubsystem->DestroySocket(Socket);
			Socket = nullptr;
			return false;
		}
	}
	Addr->SetPort(Port);

	if (!Socket->Connect(*Addr))
	{
		SocketSubsystem->DestroySocket(Socket);
		Socket = nullptr;
		return false;
	}

	UE_LOG(LogTemp, Log, TEXT("WhisperClient: Connected to %s:%d"), *Host, Port);

	// Register as listener: send locale + zero-length audio
	FTCHARToUTF8 LocaleUtf8(*Locale);
	uint8 LocaleLen = static_cast<uint8>(LocaleUtf8.Length());
	uint8 ZeroAudio[4] = {0, 0, 0, 0};
	SendAll(&LocaleLen, 1);
	SendAll(reinterpret_cast<const uint8*>(LocaleUtf8.Get()), LocaleLen);
	SendAll(ZeroAudio, 4);

	// Start the background receive thread
	bRunning = true;
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
	{
		ReceiveLoop();
	});

	return true;
}

void UWhisperClient::Disconnect()
{
	bRunning = false;

	FScopeLock Lock(&SendMutex);

	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
		Socket = nullptr;
		UE_LOG(LogTemp, Log, TEXT("WhisperClient: Disconnected"));
	}
}

bool UWhisperClient::IsConnected() const
{
	return Socket != nullptr && bRunning;
}

void UWhisperClient::SendAudio(const TArray<uint8>& PCMData)
{
	if (!IsConnected())
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnError.ExecuteIfBound(TEXT("Not connected to whisper server"));
		});
		return;
	}

	TArray<uint8> DataCopy = PCMData;

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this, DataCopy = MoveTemp(DataCopy)]()
	{
		DoSendAudio(DataCopy);
	});
}

void UWhisperClient::BeginDestroy()
{
	Disconnect();
	Super::BeginDestroy();
}

bool UWhisperClient::SendAll(const uint8* Data, int32 Length)
{
	int32 BytesSent = 0;
	while (BytesSent < Length)
	{
		int32 Sent = 0;
		if (!Socket->Send(Data + BytesSent, Length - BytesSent, Sent))
		{
			return false;
		}
		BytesSent += Sent;
	}
	return true;
}

bool UWhisperClient::RecvAll(uint8* Data, int32 Length)
{
	int32 BytesRead = 0;
	while (BytesRead < Length)
	{
		int32 Read = 0;
		if (!Socket->Recv(Data + BytesRead, Length - BytesRead, Read))
		{
			return false;
		}
		if (Read == 0)
		{
			return false;
		}
		BytesRead += Read;
	}
	return true;
}

void UWhisperClient::DoSendAudio(TArray<uint8> PCMData)
{
	FScopeLock Lock(&SendMutex);

	if (!Socket)
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnError.ExecuteIfBound(TEXT("Socket disconnected"));
		});
		return;
	}

	// Send: [1-byte locale length][locale string][4-byte big-endian audio length][PCM data]
	FTCHARToUTF8 LocaleUtf8(*Locale);
	uint8 LocaleLen = static_cast<uint8>(LocaleUtf8.Length());
	if (!SendAll(&LocaleLen, 1) || !SendAll(reinterpret_cast<const uint8*>(LocaleUtf8.Get()), LocaleLen))
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnError.ExecuteIfBound(TEXT("Failed to send locale"));
		});
		return;
	}

	uint32 PayloadLen = static_cast<uint32>(PCMData.Num());
	uint8 Header[4];
	Header[0] = (PayloadLen >> 24) & 0xFF;
	Header[1] = (PayloadLen >> 16) & 0xFF;
	Header[2] = (PayloadLen >> 8) & 0xFF;
	Header[3] = PayloadLen & 0xFF;

	if (!SendAll(Header, 4) || !SendAll(PCMData.GetData(), PCMData.Num()))
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnError.ExecuteIfBound(TEXT("Failed to send audio data"));
		});
		return;
	}
}

void UWhisperClient::ReceiveLoop()
{
	while (bRunning)
	{
		uint8 RespHeader[4];
		if (!RecvAll(RespHeader, 4))
		{
			if (bRunning)
			{
				AsyncTask(ENamedThreads::GameThread, [this]()
				{
					OnError.ExecuteIfBound(TEXT("Server disconnected"));
				});
			}
			break;
		}

		uint32 RespLen = (static_cast<uint32>(RespHeader[0]) << 24) |
		                 (static_cast<uint32>(RespHeader[1]) << 16) |
		                 (static_cast<uint32>(RespHeader[2]) << 8) |
		                  static_cast<uint32>(RespHeader[3]);

		TArray<uint8> RespBuf;
		RespBuf.SetNumUninitialized(RespLen);
		if (!RecvAll(RespBuf.GetData(), RespLen))
		{
			if (bRunning)
			{
				AsyncTask(ENamedThreads::GameThread, [this]()
				{
					OnError.ExecuteIfBound(TEXT("Failed to receive response body"));
				});
			}
			break;
		}

		FString JsonString = FString(RespLen, reinterpret_cast<const char*>(RespBuf.GetData()));
		HandleResponse(JsonString);
	}

	bRunning = false;
}

void UWhisperClient::HandleResponse(const FString& JsonString)
{
	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);

	if (!FJsonSerializer::Deserialize(Reader, JsonObject) || !JsonObject.IsValid())
	{
		AsyncTask(ENamedThreads::GameThread, [this]()
		{
			OnError.ExecuteIfBound(TEXT("Failed to parse server response"));
		});
		return;
	}

	FString ErrorMsg;
	if (JsonObject->TryGetStringField(TEXT("error"), ErrorMsg))
	{
		AsyncTask(ENamedThreads::GameThread, [this, ErrorMsg]()
		{
			OnError.ExecuteIfBound(ErrorMsg);
		});
		return;
	}

	FString Speaker = JsonObject->GetStringField(TEXT("speaker"));
	FString Redacted = JsonObject->GetStringField(TEXT("redacted"));

	AsyncTask(ENamedThreads::GameThread, [this, Speaker, Redacted]()
	{
		OnTranscriptionReceived.ExecuteIfBound(Speaker, Redacted);
	});
}
```

### VoiceChatComponent.h

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AudioCaptureCore.h"
#include "VoiceChatComponent.generated.h"

class UWhisperClient;
class UVoiceChatWidget;

/**
 * Actor component that captures mic audio on push-to-talk (Ctrl+V),
 * sends it to the Whisper transcription server, and displays the result
 * in an in-game chat widget.
 */
UCLASS(ClassGroup=(VoiceChat), meta=(BlueprintSpawnableComponent))
class UVoiceChatComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UVoiceChatComponent();

	/** Server host address. */
	UPROPERTY(EditAnywhere, Category = "Voice Chat")
	FString ServerHost = TEXT("127.0.0.1");

	/** Server port. */
	UPROPERTY(EditAnywhere, Category = "Voice Chat")
	int32 ServerPort = 9090;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void StartRecording();
	void StopRecording();
	void OnTranscriptionReceived(const FString& Speaker, const FString& Text);
	void OnError(const FString& ErrorMessage);
	TArray<uint8> ConvertToServerFormat(const TArray<float>& FloatSamples, int32 NumChannels, int32 SourceSampleRate);

	UPROPERTY()
	TObjectPtr<UWhisperClient> WhisperClient;

	Audio::FAudioCapture AudioCapture;
	bool bCaptureStreamOpen = false;

	UPROPERTY()
	TObjectPtr<UVoiceChatWidget> ChatWidget;

	bool bIsRecording = false;
	bool bKeyHeld = false;
	TArray<float> RecordingBuffer;
	int32 CaptureChannels = 1;
	int32 CaptureSampleRate = 48000;
	FCriticalSection BufferMutex;
};
```

### VoiceChatComponent.cpp

```cpp
#include "VoiceChatComponent.h"
#include "WhisperClient.h"
#include "VoiceChatWidget.h"

#include "Blueprint/UserWidget.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

UVoiceChatComponent::UVoiceChatComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UVoiceChatComponent::BeginPlay()
{
	Super::BeginPlay();

	WhisperClient = NewObject<UWhisperClient>(this);
	WhisperClient->OnTranscriptionReceived.BindUObject(this, &UVoiceChatComponent::OnTranscriptionReceived);
	WhisperClient->OnError.BindUObject(this, &UVoiceChatComponent::OnError);

	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (PC && PC->IsLocalPlayerController())
	{
		ChatWidget = CreateWidget<UVoiceChatWidget>(PC);
		if (ChatWidget)
		{
			ChatWidget->AddToViewport(10);
		}
	}

	Audio::FCaptureDeviceInfo DeviceInfo;
	if (AudioCapture.GetCaptureDeviceInfo(DeviceInfo))
	{
		CaptureSampleRate = DeviceInfo.PreferredSampleRate;
		CaptureChannels = DeviceInfo.InputChannels;
		UE_LOG(LogTemp, Log, TEXT("VoiceChat: Mic device: %s (%d Hz, %d ch)"), *DeviceInfo.DeviceName, CaptureSampleRate, CaptureChannels);
	}

	Audio::FAudioCaptureDeviceParams Params;
	Audio::FOnAudioCaptureFunction OnCapture = [this](const void* InAudio, int32 NumFrames, int32 NumChannels, int32 SampleRate, double StreamTime, bool bOverflow)
	{
		if (!bIsRecording) return;

		const float* FloatAudio = static_cast<const float*>(InAudio);
		int32 TotalSamples = NumFrames * NumChannels;
		CaptureChannels = NumChannels;
		CaptureSampleRate = SampleRate;

		FScopeLock Lock(&BufferMutex);
		RecordingBuffer.Append(FloatAudio, TotalSamples);
	};

	bCaptureStreamOpen = AudioCapture.OpenAudioCaptureStream(Params, MoveTemp(OnCapture), 1024);

	if (!bCaptureStreamOpen)
	{
		UE_LOG(LogTemp, Error, TEXT("VoiceChat: Failed to open audio capture stream"));
		if (ChatWidget) ChatWidget->ShowError(TEXT("Failed to open microphone"));
	}

	if (ChatWidget) ChatWidget->SetConnecting(true);

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
	{
		bool bConnected = WhisperClient->Connect(ServerHost, ServerPort);
		AsyncTask(ENamedThreads::GameThread, [this, bConnected]()
		{
			if (ChatWidget)
			{
				ChatWidget->SetConnecting(false);
				if (bConnected)
					ChatWidget->AddMessage(TEXT("System"), TEXT("Connected to voice server. Hold Ctrl+V to speak."));
				else
					ChatWidget->ShowError(TEXT("Could not connect to whisper server. Is it running?"));
			}
		});
	});
}

void UVoiceChatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (WhisperClient) WhisperClient->Disconnect();
	if (ChatWidget) { ChatWidget->RemoveFromParent(); ChatWidget = nullptr; }
	if (bCaptureStreamOpen) { AudioCapture.AbortStream(); AudioCapture.CloseStream(); bCaptureStreamOpen = false; }
	Super::EndPlay(EndPlayReason);
}

void UVoiceChatComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (!PC) return;

	bool bCtrlHeld = PC->IsInputKeyDown(EKeys::LeftControl) || PC->IsInputKeyDown(EKeys::RightControl);
	bool bVHeld = PC->IsInputKeyDown(EKeys::V);
	bool bPushToTalk = bCtrlHeld && bVHeld;

	if (bPushToTalk && !bKeyHeld) { bKeyHeld = true; StartRecording(); }
	else if (!bPushToTalk && bKeyHeld) { bKeyHeld = false; StopRecording(); }
}

void UVoiceChatComponent::StartRecording()
{
	if (bIsRecording || !bCaptureStreamOpen) return;
	if (!WhisperClient || !WhisperClient->IsConnected())
	{
		if (ChatWidget) ChatWidget->ShowError(TEXT("Not connected to whisper server"));
		return;
	}

	{ FScopeLock Lock(&BufferMutex); RecordingBuffer.Empty(); }
	bIsRecording = true;
	if (ChatWidget) ChatWidget->SetRecording(true);
	AudioCapture.StartStream();
	UE_LOG(LogTemp, Log, TEXT("VoiceChat: Recording started (%d Hz, %d ch)"), CaptureSampleRate, CaptureChannels);
}

void UVoiceChatComponent::StopRecording()
{
	if (!bIsRecording) return;
	bIsRecording = false;
	AudioCapture.StopStream();
	if (ChatWidget) ChatWidget->SetRecording(false);

	TArray<float> CapturedAudio;
	{ FScopeLock Lock(&BufferMutex); CapturedAudio = MoveTemp(RecordingBuffer); RecordingBuffer.Empty(); }

	if (CapturedAudio.Num() == 0) { UE_LOG(LogTemp, Warning, TEXT("VoiceChat: No audio captured")); return; }

	float Duration = static_cast<float>(CapturedAudio.Num()) / FMath::Max(CaptureChannels, 1) / FMath::Max(CaptureSampleRate, 1);
	UE_LOG(LogTemp, Log, TEXT("VoiceChat: Recording stopped. Duration: %.2fs, Samples: %d"), Duration, CapturedAudio.Num());

	TArray<uint8> PCMData = ConvertToServerFormat(CapturedAudio, CaptureChannels, CaptureSampleRate);
	if (PCMData.Num() > 0) WhisperClient->SendAudio(PCMData);
}

void UVoiceChatComponent::OnTranscriptionReceived(const FString& Speaker, const FString& Text)
{
	UE_LOG(LogTemp, Log, TEXT("VoiceChat: [%s] %s"), *Speaker, *Text);
	if (ChatWidget) ChatWidget->AddMessage(Speaker, Text);
}

void UVoiceChatComponent::OnError(const FString& ErrorMessage)
{
	UE_LOG(LogTemp, Warning, TEXT("VoiceChat Error: %s"), *ErrorMessage);
	if (ChatWidget) ChatWidget->ShowError(ErrorMessage);
}

TArray<uint8> UVoiceChatComponent::ConvertToServerFormat(const TArray<float>& FloatSamples, int32 NumChannels, int32 SourceSampleRate)
{
	constexpr int32 TargetSampleRate = 16000;

	int32 NumFrames = FloatSamples.Num() / FMath::Max(NumChannels, 1);
	TArray<float> MonoSamples;
	MonoSamples.SetNumUninitialized(NumFrames);
	for (int32 i = 0; i < NumFrames; i++)
	{
		float Sum = 0.0f;
		for (int32 ch = 0; ch < NumChannels; ch++) Sum += FloatSamples[i * NumChannels + ch];
		MonoSamples[i] = Sum / FMath::Max(NumChannels, 1);
	}

	double Ratio = static_cast<double>(TargetSampleRate) / static_cast<double>(SourceSampleRate);
	int32 OutputFrames = static_cast<int32>(NumFrames * Ratio);
	TArray<float> ResampledSamples;
	ResampledSamples.SetNumUninitialized(OutputFrames);
	for (int32 i = 0; i < OutputFrames; i++)
	{
		double SrcIndex = i / Ratio;
		int32 Idx0 = static_cast<int32>(SrcIndex);
		int32 Idx1 = FMath::Min(Idx0 + 1, NumFrames - 1);
		float Frac = static_cast<float>(SrcIndex - Idx0);
		ResampledSamples[i] = MonoSamples[Idx0] * (1.0f - Frac) + MonoSamples[Idx1] * Frac;
	}

	TArray<uint8> PCMData;
	PCMData.SetNumUninitialized(OutputFrames * sizeof(int16));
	int16* PCMPtr = reinterpret_cast<int16*>(PCMData.GetData());
	for (int32 i = 0; i < OutputFrames; i++)
	{
		float Clamped = FMath::Clamp(ResampledSamples[i], -1.0f, 1.0f);
		PCMPtr[i] = static_cast<int16>(Clamped * 32767.0f);
	}
	return PCMData;
}
```

### VoiceChatWidget.h

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "VoiceChatWidget.generated.h"

class UVerticalBox;
class UScrollBox;
class UTextBlock;
class UBorder;
class UOverlay;

/**
 * In-game chat widget that displays transcribed voice messages.
 * Shows a scrollable chat log and a recording indicator.
 */
UCLASS()
class UVoiceChatWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	void AddMessage(const FString& Speaker, const FString& Text);
	void SetRecording(bool bRecording);
	void SetConnecting(bool bConnecting);
	void ShowError(const FString& ErrorText);

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	TSharedPtr<SVerticalBox> ChatLogBox;
	TSharedPtr<STextBlock> RecordingIndicator;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SScrollBox> ScrollBox;
	static constexpr int32 MaxMessages = 50;
	int32 MessageCount = 0;
};
```

### VoiceChatWidget.cpp

```cpp
#include "VoiceChatWidget.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"

TSharedRef<SWidget> UVoiceChatWidget::RebuildWidget()
{
	RecordingIndicator = SNew(STextBlock)
		.Text(FText::FromString(TEXT("[ RECORDING ]")))
		.ColorAndOpacity(FSlateColor(FLinearColor::Red))
		.Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
		.Visibility(EVisibility::Collapsed);

	StatusText = SNew(STextBlock)
		.Text(FText::GetEmpty())
		.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.7f, 0.0f)))
		.Font(FCoreStyle::GetDefaultFontStyle("Regular", 10))
		.Visibility(EVisibility::Collapsed);

	ChatLogBox = SNew(SVerticalBox);

	ScrollBox = SNew(SScrollBox)
		.ScrollBarVisibility(EVisibility::Collapsed)
		+ SScrollBox::Slot()
		[
			ChatLogBox.ToSharedRef()
		];

	return SNew(SBox)
		.WidthOverride(450.0f)
		.MaxDesiredHeight(300.0f)
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(20.0f, 0.0f, 0.0f, 20.0f))
		[
			SNew(SBorder)
			.BorderBackgroundColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f))
			.Padding(FMargin(12.0f, 8.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().FillHeight(1.0f)
				[
					ScrollBox.ToSharedRef()
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					StatusText.ToSharedRef()
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(FMargin(0.0f, 4.0f, 0.0f, 0.0f))
				[
					RecordingIndicator.ToSharedRef()
				]
			]
		];
}

void UVoiceChatWidget::AddMessage(const FString& Speaker, const FString& Text)
{
	if (!ChatLogBox.IsValid()) return;

	if (MessageCount >= MaxMessages)
	{
		ChatLogBox->RemoveSlot(ChatLogBox->GetChildren()->GetChildAt(0));
		MessageCount--;
	}

	FString FormattedMsg = FString::Printf(TEXT("[%s]: %s"), *Speaker, *Text);

	ChatLogBox->AddSlot()
		.AutoHeight()
		.Padding(FMargin(0.0f, 2.0f))
		[
			SNew(STextBlock)
			.Text(FText::FromString(FormattedMsg))
			.ColorAndOpacity(FSlateColor(FLinearColor::White))
			.Font(FCoreStyle::GetDefaultFontStyle("Regular", 12))
			.AutoWrapText(true)
		];

	MessageCount++;
	if (ScrollBox.IsValid()) ScrollBox->ScrollToEnd();
}

void UVoiceChatWidget::SetRecording(bool bRecording)
{
	if (RecordingIndicator.IsValid())
		RecordingIndicator->SetVisibility(bRecording ? EVisibility::Visible : EVisibility::Collapsed);
}

void UVoiceChatWidget::SetConnecting(bool bConnecting)
{
	if (StatusText.IsValid())
	{
		if (bConnecting)
		{
			StatusText->SetText(FText::FromString(TEXT("Connecting to whisper server...")));
			StatusText->SetVisibility(EVisibility::Visible);
		}
		else
		{
			StatusText->SetVisibility(EVisibility::Collapsed);
		}
	}
}

void UVoiceChatWidget::ShowError(const FString& ErrorText)
{
	if (StatusText.IsValid())
	{
		StatusText->SetText(FText::FromString(ErrorText));
		StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.3f, 0.3f)));
		StatusText->SetVisibility(EVisibility::Visible);
	}
}
```

## Player Controller Integration

Add the VoiceChatComponent as a default subobject in your player controller:

**Header** (add to your existing player controller):
```cpp
#include "VoiceChatComponent.h"

// Add as a member:
UPROPERTY(VisibleAnywhere, Category = "Voice Chat")
TObjectPtr<UVoiceChatComponent> VoiceChatComponent;
```

**Constructor:**
```cpp
AYourPlayerController::AYourPlayerController()
{
	VoiceChatComponent = CreateDefaultSubobject<UVoiceChatComponent>(TEXT("VoiceChatComponent"));
}
```

That's it. The component handles everything: mic capture, server connection, push-to-talk input, and chat UI.

## How to Apply to a New Unreal Project

1. Create `Source/<ProjectName>/VoiceChat/` directory
2. Copy all 6 files (WhisperClient.h/cpp, VoiceChatComponent.h/cpp, VoiceChatWidget.h/cpp)
3. Add module dependencies to your `.Build.cs`
4. Add `"<ProjectName>/VoiceChat"` to `PublicIncludePaths`
5. Add VoiceChatComponent to your player controller
6. Build and run alongside the whisper server
