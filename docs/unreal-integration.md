# Unreal Engine Voice Chat Integration

This document contains the complete Unreal Engine C++ integration for the voice-to-chat system. These files are added to an existing Unreal Engine project to connect it to the whisper transcription server.

## Overview

The integration adds voice chat to any Unreal Engine project with:
- **Push-to-talk** via Ctrl+V
- **Language switching** via Ctrl+Shift+V — GTA V-style radial wheel with 20 languages
- **TCP connection** to the voice server on port 9090
- **Stateless locale protocol** — sends language code with every audio message
- **Broadcast support** — receives transcriptions from all connected clients
- **In-game chat widget** — semi-transparent Slate overlay at bottom-left
- **UTF-8 support** — composite font with CJK, Devanagari, Arabic, Thai fallbacks

The server handles all translation logic. It auto-detects the spoken language, runs Whisper transcribe + translate, and sends each client a simple `{"speaker","locale","text"}` message with the appropriate text already chosen (original for same-language, English for cross-language). The client just displays what it receives.

## Architecture

```
+-----------------------------------------------------+
|  Unreal Engine                                       |
|                                                      |
|  VoiceDemoPlayerController                           |
|    +-- VoiceChatComponent                            |
|          +-- AudioCapture (mic input)                |
|          +-- WhisperClient (TCP to server)            |
|          |     +-- DoSendAudio (on Ctrl+V release)   |
|          |     +-- SendLocaleUpdate (on lang change) |
|          |     +-- ReceiveLoop (background thread)   |
|          +-- VoiceChatWidget (UI overlay)             |
|          |     +-- SLanguageWheel (radial selector)  |
+-----------------------------------------------------+
         |                          ^
         | [locale][audio]          | [JSON: speaker, locale, text]
         v                          |
+-----------------------------------------------------+
|  Whisper Server (voice-unreal-chatbox)               |
|  - Auto language detection                           |
|  - Whisper transcribe + translate                    |
|  - Profanity filter                                  |
|  - Per-listener text routing                         |
+-----------------------------------------------------+
```

## Wire Protocol

```
Client -> Server:  [1-byte locale length][locale string][4-byte BE audio length][raw 16-bit PCM, 16kHz mono]
Server -> Client:  [4-byte BE length][JSON response]
```

On connect, the client sends a registration message (locale + zero-length audio) so the server adds it to the broadcast list immediately. A locale update (via the language wheel) sends another registration.

### JSON Response

The server sends a simple message with the appropriate text already chosen per listener:

```json
{"speaker":"Player1","locale":"hi","text":"There is someone behind you."}
```

- `speaker` — player ID
- `locale` — detected language of the speech
- `text` — display text (original for same-language listeners, English translation for others)

## File Structure

All voice chat files go in `Source/<ProjectName>/VoiceChat/`:

```
Source/<ProjectName>/
+-- VoiceChat/
|   +-- WhisperClient.h          # TCP client with background receive loop
|   +-- WhisperClient.cpp
|   +-- VoiceChatComponent.h     # Actor component: mic capture + push-to-talk
|   +-- VoiceChatComponent.cpp
|   +-- VoiceChatWidget.h        # In-game chat overlay widget
|   +-- VoiceChatWidget.cpp
|   +-- SLanguageWheel.h         # GTA V-style radial language selector
|   +-- SLanguageWheel.cpp
+-- <ProjectName>PlayerController.h   # Modified to add VoiceChatComponent
+-- <ProjectName>PlayerController.cpp
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

	/** Re-send registration (locale + zero-length audio) to update the server's record of this client's language. */
	void SendLocaleUpdate();

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

void UWhisperClient::SendLocaleUpdate()
{
	if (!IsConnected()) return;

	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [this]()
	{
		FScopeLock Lock(&SendMutex);
		if (!Socket) return;

		FTCHARToUTF8 LocaleUtf8(*Locale);
		uint8 LocaleLen = static_cast<uint8>(LocaleUtf8.Length());
		uint8 ZeroAudio[4] = {0, 0, 0, 0};
		SendAll(&LocaleLen, 1);
		SendAll(reinterpret_cast<const uint8*>(LocaleUtf8.Get()), LocaleLen);
		SendAll(ZeroAudio, 4);

		UE_LOG(LogTemp, Log, TEXT("WhisperClient: Sent locale update to server: %s"), *Locale);
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

		// Server sends UTF-8 JSON — must convert properly for CJK/non-ASCII text
		FUTF8ToTCHAR Converter(reinterpret_cast<const ANSICHAR*>(RespBuf.GetData()), RespLen);
		FString JsonString(Converter.Length(), Converter.Get());
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
	FString Text = JsonObject->GetStringField(TEXT("text"));

	AsyncTask(ENamedThreads::GameThread, [this, Speaker, Text]()
	{
		OnTranscriptionReceived.ExecuteIfBound(Speaker, Text);
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
 * in an in-game chat widget. Ctrl+Shift+V opens a radial language wheel.
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

	/** Supported languages. Client tells the server which language to transcribe. */
	UPROPERTY(EditAnywhere, Category = "Voice Chat")
	TArray<FString> SupportedLocales = {
		TEXT("en"), TEXT("zh"), TEXT("es"), TEXT("hi"), TEXT("ar"),
		TEXT("pt"), TEXT("ja"), TEXT("ko"), TEXT("fr"), TEXT("de"),
		TEXT("ru"), TEXT("it"), TEXT("nl"), TEXT("pl"), TEXT("tr"),
		TEXT("sv"), TEXT("th"), TEXT("vi"), TEXT("id"), TEXT("cs")
	};

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	void StartRecording();
	void StopRecording();
	void OnTranscriptionReceived(const FString& Speaker, const FString& Text);
	void OnError(const FString& ErrorMessage);
	void OnWheelConfirmed(int32 SelectedIndex);
	void OnWheelDismissed();
	TArray<uint8> ConvertToServerFormat(const TArray<float>& FloatSamples, int32 NumChannels, int32 SourceSampleRate);

	UPROPERTY()
	TObjectPtr<UWhisperClient> WhisperClient;

	Audio::FAudioCapture AudioCapture;
	bool bCaptureStreamOpen = false;

	UPROPERTY()
	TObjectPtr<UVoiceChatWidget> ChatWidget;

	bool bIsRecording = false;
	bool bKeyHeld = false;

	/** Whether the language wheel is currently open. */
	bool bWheelOpen = false;

	/** Debounce for Ctrl+Shift+V press. */
	bool bLangKeyHeld = false;

	/** Current index into SupportedLocales. */
	int32 CurrentLocaleIndex = 0;

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
			ChatWidget->OnWheelConfirmed.BindUObject(this, &UVoiceChatComponent::OnWheelConfirmed);
			ChatWidget->OnWheelDismissed.BindUObject(this, &UVoiceChatComponent::OnWheelDismissed);
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
					ChatWidget->AddMessage(TEXT("System"), TEXT("Connected. Hold Ctrl+V to speak. Ctrl+Shift+V to change language."));
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
	bool bShiftHeld = PC->IsInputKeyDown(EKeys::LeftShift) || PC->IsInputKeyDown(EKeys::RightShift);
	bool bVHeld = PC->IsInputKeyDown(EKeys::V);

	// Ctrl+Shift+V = toggle language wheel
	bool bLangSelectPressed = bCtrlHeld && bShiftHeld && bVHeld;
	if (bLangSelectPressed && !bLangKeyHeld)
	{
		bLangKeyHeld = true;
		if (!bWheelOpen && ChatWidget)
		{
			bWheelOpen = true;
			ChatWidget->ShowLanguageWheel(SupportedLocales, CurrentLocaleIndex);

			// Show cursor so the player can interact with the wheel
			PC->bShowMouseCursor = true;
			PC->SetInputMode(FInputModeGameAndUI().SetHideCursorDuringCapture(false));
		}
	}
	else if (!bLangSelectPressed)
	{
		bLangKeyHeld = false;
	}

	// Ctrl+V (without Shift) = push to talk (only when wheel is closed)
	if (!bWheelOpen)
	{
		bool bPushToTalk = bCtrlHeld && bVHeld && !bShiftHeld;

		if (bPushToTalk && !bKeyHeld) { bKeyHeld = true; StartRecording(); }
		else if (!bPushToTalk && bKeyHeld) { bKeyHeld = false; StopRecording(); }
	}
}

void UVoiceChatComponent::OnWheelConfirmed(int32 SelectedIndex)
{
	if (SupportedLocales.IsValidIndex(SelectedIndex))
	{
		CurrentLocaleIndex = SelectedIndex;
		FString NewLocale = SupportedLocales[CurrentLocaleIndex];
		if (WhisperClient)
		{
			WhisperClient->Locale = NewLocale;
			WhisperClient->SendLocaleUpdate();
		}
		UE_LOG(LogTemp, Log, TEXT("VoiceChat: Language switched to %s"), *NewLocale);
		if (ChatWidget)
		{
			ChatWidget->AddMessage(TEXT("System"), FString::Printf(TEXT("Language set to: %s"), *NewLocale));
		}
	}

	// Close wheel and restore game input
	bWheelOpen = false;
	if (ChatWidget) ChatWidget->HideLanguageWheel();

	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (PC)
	{
		PC->bShowMouseCursor = false;
		PC->SetInputMode(FInputModeGameOnly());
	}
}

void UVoiceChatComponent::OnWheelDismissed()
{
	bWheelOpen = false;
	if (ChatWidget) ChatWidget->HideLanguageWheel();

	APlayerController* PC = Cast<APlayerController>(GetOwner());
	if (PC)
	{
		PC->bShowMouseCursor = false;
		PC->SetInputMode(FInputModeGameOnly());
	}
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
class SLanguageWheel;

/**
 * In-game chat widget that displays transcribed voice messages.
 * Shows a scrollable chat log, recording indicator, and language wheel.
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

	/** Show the radial language wheel. */
	void ShowLanguageWheel(const TArray<FString>& Locales, int32 CurrentIndex);

	/** Hide the language wheel. */
	void HideLanguageWheel();

	/** Returns true if the wheel is currently visible. */
	bool IsWheelVisible() const;

	/** Scroll the wheel selection by delta (+1 or -1). */
	void ScrollWheel(int32 Delta);

	/** Confirm the current wheel selection. Returns the selected index, or -1 if wheel not open. */
	int32 ConfirmWheelSelection();

	/** Dismiss wheel without changing language. */
	void DismissWheel();

	DECLARE_DELEGATE_OneParam(FOnWheelConfirmed, int32 /*SelectedIndex*/);
	FOnWheelConfirmed OnWheelConfirmed;

	DECLARE_DELEGATE(FOnWheelDismissed);
	FOnWheelDismissed OnWheelDismissed;

protected:
	virtual TSharedRef<SWidget> RebuildWidget() override;

private:
	TSharedPtr<SVerticalBox> ChatLogBox;
	TSharedPtr<STextBlock> RecordingIndicator;
	TSharedPtr<STextBlock> StatusText;
	TSharedPtr<SScrollBox> ScrollBox;
	TSharedPtr<SLanguageWheel> LanguageWheel;
	TSharedPtr<SBox> WheelContainer;
	static constexpr int32 MaxMessages = 50;
	int32 MessageCount = 0;
};
```

### VoiceChatWidget.cpp

```cpp
#include "VoiceChatWidget.h"
#include "SLanguageWheel.h"

#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Misc/Paths.h"

// Helper: add an inclusive Unicode range to a sub-font
static void AddRange(FCompositeSubFont& Sub, int32 Lo, int32 Hi)
{
	Sub.CharacterRanges.Add(FInt32Range(FInt32RangeBound::Inclusive(Lo), FInt32RangeBound::Inclusive(Hi)));
}

// Build a composite font with Noto Sans as default + CJK/Devanagari/Arabic fallbacks.
static FSlateFontInfo GetChatFont(int32 Size)
{
	static TSharedPtr<FCompositeFont> ChatFont;
	if (!ChatFont.IsValid())
	{
		FString FontDir = FPaths::ProjectContentDir() / TEXT("Fonts");

		ChatFont = MakeShared<FCompositeFont>();

		// Default typeface: Noto Sans (Latin, Cyrillic, Greek)
		{
			FTypefaceEntry& Entry = ChatFont->DefaultTypeface.Fonts[ChatFont->DefaultTypeface.Fonts.AddDefaulted()];
			Entry.Name = TEXT("Regular");
			Entry.Font = FFontData(FontDir / TEXT("NotoSans-Regular.ttf"), EFontHinting::Auto, EFontLoadingPolicy::LazyLoad);
		}

		// CJK (Japanese, Chinese, Korean)
		{
			FCompositeSubFont& Sub = ChatFont->SubTypefaces[ChatFont->SubTypefaces.AddDefaulted()];
			AddRange(Sub, 0x2E80, 0x2EFF);   // CJK Radicals Supplement
			AddRange(Sub, 0x2F00, 0x2FDF);   // Kangxi Radicals
			AddRange(Sub, 0x3000, 0x303F);   // CJK Symbols and Punctuation
			AddRange(Sub, 0x3040, 0x309F);   // Hiragana
			AddRange(Sub, 0x30A0, 0x30FF);   // Katakana
			AddRange(Sub, 0x31F0, 0x31FF);   // Katakana Phonetic Extensions
			AddRange(Sub, 0x3200, 0x32FF);   // Enclosed CJK Letters and Months
			AddRange(Sub, 0x3300, 0x33FF);   // CJK Compatibility
			AddRange(Sub, 0x3400, 0x4DBF);   // CJK Unified Ideographs Extension A
			AddRange(Sub, 0x4E00, 0x9FFF);   // CJK Unified Ideographs
			AddRange(Sub, 0xAC00, 0xD7AF);   // Hangul Syllables
			AddRange(Sub, 0x1100, 0x11FF);   // Hangul Jamo
			AddRange(Sub, 0x3130, 0x318F);   // Hangul Compatibility Jamo
			AddRange(Sub, 0xF900, 0xFAFF);   // CJK Compatibility Ideographs
			AddRange(Sub, 0xFE30, 0xFE4F);   // CJK Compatibility Forms
			AddRange(Sub, 0xFF00, 0xFFEF);   // Halfwidth and Fullwidth Forms
			FTypefaceEntry& Entry = Sub.Typeface.Fonts[Sub.Typeface.Fonts.AddDefaulted()];
			Entry.Name = TEXT("CJK");
			Entry.Font = FFontData(FontDir / TEXT("NotoSansCJKjp-Regular.otf"), EFontHinting::Auto, EFontLoadingPolicy::LazyLoad);
		}

		// Devanagari (Hindi)
		{
			FCompositeSubFont& Sub = ChatFont->SubTypefaces[ChatFont->SubTypefaces.AddDefaulted()];
			AddRange(Sub, 0x0900, 0x097F);   // Devanagari
			AddRange(Sub, 0xA8E0, 0xA8FF);   // Devanagari Extended
			FTypefaceEntry& Entry = Sub.Typeface.Fonts[Sub.Typeface.Fonts.AddDefaulted()];
			Entry.Name = TEXT("Devanagari");
			Entry.Font = FFontData(FontDir / TEXT("NotoSansDevanagari-Regular.ttf"), EFontHinting::Auto, EFontLoadingPolicy::LazyLoad);
		}

		// Arabic
		{
			FCompositeSubFont& Sub = ChatFont->SubTypefaces[ChatFont->SubTypefaces.AddDefaulted()];
			AddRange(Sub, 0x0600, 0x06FF);   // Arabic
			AddRange(Sub, 0x0750, 0x077F);   // Arabic Supplement
			AddRange(Sub, 0xFB50, 0xFDFF);   // Arabic Presentation Forms-A
			AddRange(Sub, 0xFE70, 0xFEFF);   // Arabic Presentation Forms-B
			FTypefaceEntry& Entry = Sub.Typeface.Fonts[Sub.Typeface.Fonts.AddDefaulted()];
			Entry.Name = TEXT("Arabic");
			Entry.Font = FFontData(FontDir / TEXT("NotoSansArabic-Regular.ttf"), EFontHinting::Auto, EFontLoadingPolicy::LazyLoad);
		}

		// Thai (Noto Sans covers Thai)
		{
			FCompositeSubFont& Sub = ChatFont->SubTypefaces[ChatFont->SubTypefaces.AddDefaulted()];
			AddRange(Sub, 0x0E00, 0x0E7F);   // Thai
			FTypefaceEntry& Entry = Sub.Typeface.Fonts[Sub.Typeface.Fonts.AddDefaulted()];
			Entry.Name = TEXT("Thai");
			Entry.Font = FFontData(FontDir / TEXT("NotoSans-Regular.ttf"), EFontHinting::Auto, EFontLoadingPolicy::LazyLoad);
		}

		UE_LOG(LogTemp, Log, TEXT("VoiceChat: Loaded composite font from %s"), *FontDir);
	}

	return FSlateFontInfo(ChatFont.ToSharedRef(), Size);
}

TSharedRef<SWidget> UVoiceChatWidget::RebuildWidget()
{
	RecordingIndicator = SNew(STextBlock)
		.Text(FText::FromString(TEXT("[ RECORDING ]")))
		.ColorAndOpacity(FSlateColor(FLinearColor::Red))
		.Font(GetChatFont(14))
		.Visibility(EVisibility::Collapsed);

	StatusText = SNew(STextBlock)
		.Text(FText::GetEmpty())
		.ColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.7f, 0.0f)))
		.Font(GetChatFont(10))
		.Visibility(EVisibility::Collapsed);

	ChatLogBox = SNew(SVerticalBox);

	ScrollBox = SNew(SScrollBox)
		.ScrollBarVisibility(EVisibility::Collapsed)
		+ SScrollBox::Slot()
		[
			ChatLogBox.ToSharedRef()
		];

	// Wheel container — centered on screen, hidden by default
	WheelContainer = SNew(SBox)
		.WidthOverride(400.0f)
		.HeightOverride(400.0f)
		.Visibility(EVisibility::Collapsed);

	return SNew(SOverlay)
		// Chat log at bottom-left
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Bottom)
		.Padding(FMargin(20.0f, 0.0f, 0.0f, 20.0f))
		[
			SNew(SBox)
			.WidthOverride(450.0f)
			.MaxDesiredHeight(300.0f)
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
			]
		]
		// Language wheel centered on screen
		+ SOverlay::Slot()
		.HAlign(HAlign_Center)
		.VAlign(VAlign_Center)
		[
			WheelContainer.ToSharedRef()
		];
}

void UVoiceChatWidget::ShowLanguageWheel(const TArray<FString>& Locales, int32 CurrentIndex)
{
	if (!WheelContainer.IsValid()) return;

	LanguageWheel = SNew(SLanguageWheel)
		.Locales(Locales)
		.SelectedIndex(CurrentIndex)
		.OnLanguageSelected_Lambda([this](int32 Index)
		{
			OnWheelConfirmed.ExecuteIfBound(Index);
		})
		.OnWheelDismissed_Lambda([this]()
		{
			OnWheelDismissed.ExecuteIfBound();
		});

	WheelContainer->SetContent(LanguageWheel.ToSharedRef());
	WheelContainer->SetVisibility(EVisibility::Visible);

	// Give the wheel keyboard/mouse focus
	FSlateApplication::Get().SetKeyboardFocus(LanguageWheel);
}

void UVoiceChatWidget::HideLanguageWheel()
{
	if (WheelContainer.IsValid())
	{
		WheelContainer->SetVisibility(EVisibility::Collapsed);
		WheelContainer->SetContent(SNullWidget::NullWidget);
	}
	LanguageWheel.Reset();
}

bool UVoiceChatWidget::IsWheelVisible() const
{
	return WheelContainer.IsValid() && WheelContainer->GetVisibility() == EVisibility::Visible;
}

void UVoiceChatWidget::ScrollWheel(int32 Delta)
{
	if (LanguageWheel.IsValid())
	{
		LanguageWheel->ScrollSelection(Delta);
	}
}

int32 UVoiceChatWidget::ConfirmWheelSelection()
{
	if (LanguageWheel.IsValid())
	{
		return LanguageWheel->GetSelectedIndex();
	}
	return -1;
}

void UVoiceChatWidget::DismissWheel()
{
	HideLanguageWheel();
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
			.Font(GetChatFont(12))
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

### SLanguageWheel.h

```cpp
#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/DeclarativeSyntaxSupport.h"

class FSlateWindowElementList;

/**
 * GTA V-style radial language selector.
 * Draws a semi-transparent wheel with language codes arranged in a circle.
 * Mouse scroll cycles selection, left-click confirms.
 */
class SLanguageWheel : public SCompoundWidget
{
public:
	DECLARE_DELEGATE_OneParam(FOnLanguageSelected, int32 /*SelectedIndex*/);
	DECLARE_DELEGATE(FOnWheelDismissed);

	SLATE_BEGIN_ARGS(SLanguageWheel)
		: _Locales()
		, _SelectedIndex(0)
	{}
		SLATE_ARGUMENT(TArray<FString>, Locales)
		SLATE_ARGUMENT(int32, SelectedIndex)
		SLATE_EVENT(FOnLanguageSelected, OnLanguageSelected)
		SLATE_EVENT(FOnWheelDismissed, OnWheelDismissed)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetSelectedIndex(int32 NewIndex);
	int32 GetSelectedIndex() const { return SelectedIndex; }
	void ScrollSelection(int32 Delta);

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FVector2D ComputeDesiredSize(float LayoutScaleMultiplier) const override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual bool SupportsKeyboardFocus() const override { return true; }

private:
	/** Full display names for the locale codes. */
	static FString GetLanguageDisplayName(const FString& Locale);

	TArray<FString> Locales;
	int32 SelectedIndex = 0;
	FOnLanguageSelected OnLanguageSelected;
	FOnWheelDismissed OnWheelDismissed;

	static constexpr float WheelRadius = 150.0f;
	static constexpr float CenterRadius = 45.0f;
	static constexpr float WheelSize = 400.0f;
};
```

### SLanguageWheel.cpp

```cpp
#include "SLanguageWheel.h"
#include "Rendering/DrawElements.h"
#include "Fonts/FontMeasure.h"
#include "Styling/CoreStyle.h"
#include "Framework/Application/SlateApplication.h"

void SLanguageWheel::Construct(const FArguments& InArgs)
{
	Locales = InArgs._Locales;
	SelectedIndex = InArgs._SelectedIndex;
	OnLanguageSelected = InArgs._OnLanguageSelected;
	OnWheelDismissed = InArgs._OnWheelDismissed;
}

void SLanguageWheel::SetSelectedIndex(int32 NewIndex)
{
	if (Locales.Num() > 0)
	{
		SelectedIndex = ((NewIndex % Locales.Num()) + Locales.Num()) % Locales.Num();
	}
}

void SLanguageWheel::ScrollSelection(int32 Delta)
{
	SetSelectedIndex(SelectedIndex + Delta);
}

FVector2D SLanguageWheel::ComputeDesiredSize(float LayoutScaleMultiplier) const
{
	return FVector2D(WheelSize, WheelSize);
}

FString SLanguageWheel::GetLanguageDisplayName(const FString& Locale)
{
	if (Locale == TEXT("en")) return TEXT("English");
	if (Locale == TEXT("zh")) return TEXT("Chinese");
	if (Locale == TEXT("es")) return TEXT("Spanish");
	if (Locale == TEXT("hi")) return TEXT("Hindi");
	if (Locale == TEXT("ar")) return TEXT("Arabic");
	if (Locale == TEXT("pt")) return TEXT("Portuguese");
	if (Locale == TEXT("ja")) return TEXT("Japanese");
	if (Locale == TEXT("ko")) return TEXT("Korean");
	if (Locale == TEXT("fr")) return TEXT("French");
	if (Locale == TEXT("de")) return TEXT("German");
	if (Locale == TEXT("ru")) return TEXT("Russian");
	if (Locale == TEXT("it")) return TEXT("Italian");
	if (Locale == TEXT("nl")) return TEXT("Dutch");
	if (Locale == TEXT("pl")) return TEXT("Polish");
	if (Locale == TEXT("tr")) return TEXT("Turkish");
	if (Locale == TEXT("sv")) return TEXT("Swedish");
	if (Locale == TEXT("th")) return TEXT("Thai");
	if (Locale == TEXT("vi")) return TEXT("Vietnamese");
	if (Locale == TEXT("id")) return TEXT("Indonesian");
	if (Locale == TEXT("cs")) return TEXT("Czech");
	return Locale.ToUpper();
}

int32 SLanguageWheel::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	const FVector2D Center = AllottedGeometry.GetLocalSize() * 0.5f;
	const int32 NumLocales = Locales.Num();
	if (NumLocales == 0)
	{
		return LayerId;
	}

	const FSlateBrush* WhiteBrush = FCoreStyle::Get().GetBrush("WhiteBrush");

	// Draw outer ring background
	{
		constexpr int32 CircleSegments = 64;
		TArray<FSlateVertex> Vertices;
		TArray<SlateIndex> Indices;

		FSlateVertex CenterVert;
		CenterVert.Position = FVector2f(Center.X, Center.Y);
		CenterVert.Color = FColor(0, 0, 0, 180);
		CenterVert.TexCoords[0] = 0.5f;
		CenterVert.TexCoords[1] = 0.5f;
		CenterVert.TexCoords[2] = 0.0f;
		CenterVert.TexCoords[3] = 0.0f;
		Vertices.Add(CenterVert);

		for (int32 i = 0; i <= CircleSegments; i++)
		{
			float Angle = 2.0f * PI * i / CircleSegments;
			FSlateVertex Vert;
			Vert.Position = FVector2f(
				Center.X + WheelRadius * 1.15f * FMath::Cos(Angle),
				Center.Y + WheelRadius * 1.15f * FMath::Sin(Angle));
			Vert.Color = FColor(0, 0, 0, 140);
			Vert.TexCoords[0] = 0.5f + 0.5f * FMath::Cos(Angle);
			Vert.TexCoords[1] = 0.5f + 0.5f * FMath::Sin(Angle);
			Vert.TexCoords[2] = 0.0f;
			Vert.TexCoords[3] = 0.0f;
			Vertices.Add(Vert);

			if (i > 0)
			{
				Indices.Add(0);
				Indices.Add(i);
				Indices.Add(i + 1);
			}
		}

		FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, WhiteBrush->GetRenderingResource(),
			Vertices, Indices, nullptr, 0, 0);
	}

	LayerId++;

	// Draw highlight for selected segment
	const float AnglePerSegment = 2.0f * PI / NumLocales;
	const float StartOffset = -PI / 2.0f;

	{
		float SegStart = StartOffset + SelectedIndex * AnglePerSegment;
		float SegEnd = SegStart + AnglePerSegment;
		constexpr int32 HighlightSegments = 20;

		TArray<FSlateVertex> Vertices;
		TArray<SlateIndex> Indices;

		FSlateVertex CenterVert;
		CenterVert.Position = FVector2f(Center.X, Center.Y);
		CenterVert.Color = FColor(60, 160, 255, 100);
		CenterVert.TexCoords[0] = 0.5f;
		CenterVert.TexCoords[1] = 0.5f;
		CenterVert.TexCoords[2] = 0.0f;
		CenterVert.TexCoords[3] = 0.0f;
		Vertices.Add(CenterVert);

		for (int32 i = 0; i <= HighlightSegments; i++)
		{
			float Angle = SegStart + (SegEnd - SegStart) * i / HighlightSegments;
			FSlateVertex Vert;
			Vert.Position = FVector2f(
				Center.X + WheelRadius * 1.15f * FMath::Cos(Angle),
				Center.Y + WheelRadius * 1.15f * FMath::Sin(Angle));
			Vert.Color = FColor(60, 160, 255, 80);
			Vert.TexCoords[0] = 0.5f + 0.5f * FMath::Cos(Angle);
			Vert.TexCoords[1] = 0.5f + 0.5f * FMath::Sin(Angle);
			Vert.TexCoords[2] = 0.0f;
			Vert.TexCoords[3] = 0.0f;
			Vertices.Add(Vert);

			if (i > 0)
			{
				Indices.Add(0);
				Indices.Add(i);
				Indices.Add(i + 1);
			}
		}

		FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, WhiteBrush->GetRenderingResource(),
			Vertices, Indices, nullptr, 0, 0);
	}

	LayerId++;

	// Draw divider lines between segments
	for (int32 i = 0; i < NumLocales; i++)
	{
		float Angle = StartOffset + i * AnglePerSegment;
		FVector2D LineEnd(
			Center.X + WheelRadius * 1.15f * FMath::Cos(Angle),
			Center.Y + WheelRadius * 1.15f * FMath::Sin(Angle));

		TArray<FVector2D> Points;
		Points.Add(Center);
		Points.Add(LineEnd);
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
			Points, ESlateDrawEffect::None, FLinearColor(1.0f, 1.0f, 1.0f, 0.15f), true, 1.0f);
	}

	LayerId++;

	// Draw language labels around the wheel
	const FSlateFontInfo CodeFont = FCoreStyle::GetDefaultFontStyle("Bold", 16);
	const FSlateFontInfo NameFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);
	TSharedRef<FSlateFontMeasure> FontMeasure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

	for (int32 i = 0; i < NumLocales; i++)
	{
		float MidAngle = StartOffset + (i + 0.5f) * AnglePerSegment;
		float LabelRadius = WheelRadius * 0.72f;
		FVector2D LabelPos(
			Center.X + LabelRadius * FMath::Cos(MidAngle),
			Center.Y + LabelRadius * FMath::Sin(MidAngle));

		bool bSelected = (i == SelectedIndex);
		FLinearColor CodeColor = bSelected ? FLinearColor(0.3f, 0.75f, 1.0f) : FLinearColor(0.85f, 0.85f, 0.85f);
		FLinearColor NameColor = bSelected ? FLinearColor(0.6f, 0.85f, 1.0f) : FLinearColor(0.5f, 0.5f, 0.5f);

		FString CodeStr = Locales[i].ToUpper();
		FString NameStr = GetLanguageDisplayName(Locales[i]);

		FVector2D CodeSize = FontMeasure->Measure(CodeStr, CodeFont);
		FVector2D NameSize = FontMeasure->Measure(NameStr, NameFont);

		float TotalHeight = CodeSize.Y + NameSize.Y + 2.0f;
		FVector2D CodePos(LabelPos.X - CodeSize.X * 0.5f, LabelPos.Y - TotalHeight * 0.5f);
		FVector2D NamePos(LabelPos.X - NameSize.X * 0.5f, LabelPos.Y - TotalHeight * 0.5f + CodeSize.Y + 2.0f);

		FSlateDrawElement::MakeText(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(CodeSize, FSlateLayoutTransform(CodePos)),
			CodeStr, CodeFont, ESlateDrawEffect::None, CodeColor);
		FSlateDrawElement::MakeText(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(NameSize, FSlateLayoutTransform(NamePos)),
			NameStr, NameFont, ESlateDrawEffect::None, NameColor);
	}

	LayerId++;

	// Draw center circle with current selection
	{
		constexpr int32 CircleSegments = 32;
		TArray<FSlateVertex> Vertices;
		TArray<SlateIndex> Indices;

		FSlateVertex CenterVert;
		CenterVert.Position = FVector2f(Center.X, Center.Y);
		CenterVert.Color = FColor(20, 20, 20, 220);
		CenterVert.TexCoords[0] = 0.5f;
		CenterVert.TexCoords[1] = 0.5f;
		CenterVert.TexCoords[2] = 0.0f;
		CenterVert.TexCoords[3] = 0.0f;
		Vertices.Add(CenterVert);

		for (int32 i = 0; i <= CircleSegments; i++)
		{
			float Angle = 2.0f * PI * i / CircleSegments;
			FSlateVertex Vert;
			Vert.Position = FVector2f(
				Center.X + CenterRadius * FMath::Cos(Angle),
				Center.Y + CenterRadius * FMath::Sin(Angle));
			Vert.Color = FColor(30, 30, 30, 220);
			Vert.TexCoords[0] = 0.5f + 0.5f * FMath::Cos(Angle);
			Vert.TexCoords[1] = 0.5f + 0.5f * FMath::Sin(Angle);
			Vert.TexCoords[2] = 0.0f;
			Vert.TexCoords[3] = 0.0f;
			Vertices.Add(Vert);

			if (i > 0)
			{
				Indices.Add(0);
				Indices.Add(i);
				Indices.Add(i + 1);
			}
		}

		FSlateDrawElement::MakeCustomVerts(OutDrawElements, LayerId, WhiteBrush->GetRenderingResource(),
			Vertices, Indices, nullptr, 0, 0);
	}

	LayerId++;

	// Center label — selected language code
	if (Locales.IsValidIndex(SelectedIndex))
	{
		const FSlateFontInfo CenterFont = FCoreStyle::GetDefaultFontStyle("Bold", 20);
		FString CenterStr = Locales[SelectedIndex].ToUpper();
		FVector2D TextSize = FontMeasure->Measure(CenterStr, CenterFont);
		FVector2D TextPos(Center.X - TextSize.X * 0.5f, Center.Y - TextSize.Y * 0.5f);

		FSlateDrawElement::MakeText(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(TextSize, FSlateLayoutTransform(TextPos)),
			CenterStr, CenterFont, ESlateDrawEffect::None, FLinearColor::White);
	}

	return LayerId;
}

FReply SLanguageWheel::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	int32 Delta = MouseEvent.GetWheelDelta() > 0 ? -1 : 1;
	ScrollSelection(Delta);
	return FReply::Handled();
}

FReply SLanguageWheel::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		OnLanguageSelected.ExecuteIfBound(SelectedIndex);
		return FReply::Handled();
	}
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		OnWheelDismissed.ExecuteIfBound();
		return FReply::Handled();
	}
	return FReply::Unhandled();
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

That's it. The component handles everything: mic capture, server connection, push-to-talk input, language selection, and chat UI.

## How to Apply to a New Unreal Project

1. Create `Source/<ProjectName>/VoiceChat/` directory
2. Copy all 8 files (WhisperClient, VoiceChatComponent, VoiceChatWidget, SLanguageWheel — .h and .cpp each)
3. Add module dependencies to your `.Build.cs`
4. Add `"<ProjectName>/VoiceChat"` to `PublicIncludePaths`
5. Add VoiceChatComponent to your player controller
6. Download Noto Sans fonts (Regular, CJK, Devanagari, Arabic) to `Content/Fonts/`
7. Build and run alongside the whisper server
