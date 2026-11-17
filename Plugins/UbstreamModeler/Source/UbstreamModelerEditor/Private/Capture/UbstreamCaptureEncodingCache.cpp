#include "Capture/UbstreamCaptureEncodingCache.h"

#include "ImageUtils.h"
#include "Misc/ScopeLock.h"

bool FUbstreamCaptureEncodingCache::Compress(FImage&& Image, const int32 Quality, const bool bDepth, TArray64<uint8>& OutBytes)
{
	FScopeLock Lock(&Mutex);
	FEntry& Entry = bDepth ? Depth : Color;
	if (!Entry.Bytes.IsEmpty() && Entry.Quality == Quality
		&& Entry.Image.SizeX == Image.SizeX && Entry.Image.SizeY == Image.SizeY
		&& Entry.Image.NumSlices == Image.NumSlices && Entry.Image.Format == Image.Format
		&& Entry.Image.GammaSpace == Image.GammaSpace && Entry.Image.RawData == Image.RawData)
	{
		OutBytes = Entry.Bytes;
		return true;
	}
	if (!FImageUtils::CompressImage(OutBytes, TEXT("jpg"), Image, Quality))
	{
		return false;
	}
#if WITH_DEV_AUTOMATION_TESTS
	++CompressionCount;
#endif
	// Move the raw buffer that capture already owns instead of adding a full
	// image copy to every changing frame. Only compressed bytes are copied.
	Entry.Image = MoveTemp(Image);
	Entry.Quality = Quality;
	Entry.Bytes = OutBytes;
	return true;
}
