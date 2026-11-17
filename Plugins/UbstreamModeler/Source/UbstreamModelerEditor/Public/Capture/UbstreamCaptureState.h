#pragma once

#include "CoreMinimal.h"

/** Immutable identity of the scene when capture began, before GPU readback. */
struct FUbstreamCaptureTicket
{
	uint64 Generation = 0;
	uint64 Sequence = 0;
	uint64 Revision = 0;
	uint64 SceneSignature = 0;
};

/** Game-thread capture lifecycle, independent of Slate, rendering and transport. */
class FUbstreamCaptureState
{
public:
	FUbstreamCaptureTicket Snapshot(uint64 SceneSignature)
	{
		return {Generation, ++Sequence, Revision, SceneSignature};
	}

	FUbstreamCaptureTicket BeginAsync(uint64 SceneSignature)
	{
		check(!ActiveCapture.IsSet());
		ActiveCapture = Snapshot(SceneSignature);
		return ActiveCapture.GetValue();
	}

	bool IsBusy() const { return ActiveCapture.IsSet(); }
	const FUbstreamCaptureTicket& GetActiveTicket() const { return ActiveCapture.GetValue(); }
	bool IsCurrent(const FUbstreamCaptureTicket& Ticket) const { return Ticket.Generation == Generation; }

	bool FinishAsync(const FUbstreamCaptureTicket& Ticket)
	{
		if (!IsCurrent(Ticket) || !ActiveCapture.IsSet() || Ticket.Sequence != ActiveCapture->Sequence)
		{
			return false;
		}
		ActiveCapture.Reset();
		return true;
	}

	void Cancel()
	{
		++Generation;
		ActiveCapture.Reset();
		MarkDirty();
	}

	void MarkDirty() { ++Revision; }
	bool IsDirty() const { return !bHasUploaded || UploadedRevision != Revision; }
	bool HasSceneChanged(uint64 Signature) const { return !bHasUploaded || Signature != UploadedSignature; }
	bool HasUploaded() const { return bHasUploaded; }

	void ResetUploads()
	{
		bHasUploaded = false;
		MarkDirty();
	}

	void RememberUploaded(const FUbstreamCaptureTicket& Ticket, int32 ColorBytes)
	{
		if (!IsCurrent(Ticket))
		{
			return;
		}
		bHasUploaded = true;
		UploadedRevision = Ticket.Revision;
		UploadedSignature = Ticket.SceneSignature;
		UploadedColorBytes = ColorBytes;
	}

	bool IsDuplicate(const FUbstreamCaptureTicket& Ticket, int32 ColorBytes) const
	{
		return IsCurrent(Ticket) && bHasUploaded
			&& Ticket.Revision == UploadedRevision
			&& Ticket.SceneSignature == UploadedSignature && ColorBytes == UploadedColorBytes;
	}

	/** Only UBS1 releases its send window with acknowledgements. */
	static bool HasSendCapacity(bool bFastStream, int32 OutstandingFrames, int32 MaxInFlightFrames)
	{
		return bFastStream || OutstandingFrames < FMath::Max(1, MaxInFlightFrames);
	}

private:
	TOptional<FUbstreamCaptureTicket> ActiveCapture;
	uint64 Generation = 0;
	uint64 Sequence = 0;
	uint64 Revision = 0;
	uint64 UploadedRevision = 0;
	uint64 UploadedSignature = 0;
	int32 UploadedColorBytes = 0;
	bool bHasUploaded = false;
};
