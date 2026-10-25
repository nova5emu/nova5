using System.Buffers.Binary;
using System.Runtime.InteropServices;
using SDL;
using static SDL.SDL3;

namespace Nova5.Services;

// Decodes a game's sce_sys/snd0.at9 (ATRAC9 in RIFF) and loops it through SDL, with fades between games.
public sealed unsafe class MenuMusic : IDisposable
{
	private sealed record Track(short[] Pcm, int Channels, int Rate, int LoopStart, int LoopEnd);

	private readonly object       _lock = new();
	private readonly Thread       _feeder;
	private SDL_AudioStream*      _stream;
	private Track?                _track;
	private int                   _cursor;
	private float                 _gain;
	private float                 _target;
	private string?               _pending;
	private CancellationTokenSource? _decode;
	private volatile bool         _running = true;

	public double Volume { get; set; } = 0.6;
	public bool   Enabled { get; set; } = true;

	public MenuMusic()
	{
		_feeder = new Thread(Feed) { IsBackground = true, Name = "Nova5 music" };
		_feeder.Start();
	}

	// Fades out the current track, then decodes and fades in the new one. Null stops the music.
	public void Play(string? at9Path)
	{
		lock (_lock)
		{
			if (at9Path == _pending) return;
			_pending = at9Path;
			_track   = null;
			_target  = 0;
		}
		_decode?.Cancel();
		if (at9Path is null || !Enabled) return;
		var cancel = _decode = new CancellationTokenSource();
		Task.Run(() =>
		{
			try
			{
				Thread.Sleep(350); // Browsing quickly past several games should not decode each of them.
				if (cancel.IsCancellationRequested) return;
				var track = Decode(at9Path);
				if (track is null || cancel.IsCancellationRequested) return;
				lock (_lock)
				{
					if (_pending != at9Path) return;
					_track = track;
				}
			}
			catch (Exception e) when (e is IOException or InvalidDataException or DllNotFoundException or EntryPointNotFoundException)
			{
			}
		});
	}

	public void Stop() => Play(null);

	private void Feed()
	{
		Track? playing = null;
		while (_running)
		{
			Thread.Sleep(20);
			lock (_lock)
			{
				// Swap tracks only once the previous one has faded out.
				if (_track != playing && _gain <= 0.001f)
				{
					Close();
					playing = _track;
					_cursor = 0;
					if (playing is not null && Open(playing)) _target = 1;
				}
				if (_stream is null || playing is null) continue;
				var step  = 0.02f / (_target > _gain ? 1.2f : 0.35f);
				_gain     = _target > _gain ? MathF.Min(_target, _gain + step) : MathF.Max(_target, _gain - step);
				SDL_SetAudioStreamGain(_stream, _gain * (float)(Enabled ? Volume : 0));
				var bytesPerSecond = playing.Rate * playing.Channels * 2;
				while (SDL_GetAudioStreamQueued(_stream) < bytesPerSecond / 3)
				{
					var end   = playing.LoopEnd;
					var count = Math.Min(end - _cursor, playing.Rate / 10) * playing.Channels;
					fixed (short* data = &playing.Pcm[_cursor * playing.Channels])
					{
						SDL_PutAudioStreamData(_stream, (IntPtr)data, count * 2);
					}
					_cursor += count / playing.Channels;
					if (_cursor >= end) _cursor = playing.LoopStart;
				}
			}
		}
	}

	private bool Open(Track track)
	{
		var spec = new SDL_AudioSpec { format = SDL_AudioFormat.SDL_AUDIO_S16LE, channels = track.Channels, freq = track.Rate };
		_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, null, IntPtr.Zero);
		if (_stream is null) return false;
		SDL_SetAudioStreamGain(_stream, 0);
		SDL_ResumeAudioStreamDevice(_stream);
		return true;
	}

	private void Close()
	{
		if (_stream is null) return;
		SDL_DestroyAudioStream(_stream);
		_stream = null;
	}

	// --music-test <snd0.at9> <out.wav>: decodes a menu track to WAV to check the ATRAC9 path.
	public static bool ExportWav(string at9Path, string wavPath)
	{
		if (Decode(at9Path) is not { } track) return false;
		using var writer = new BinaryWriter(File.Create(wavPath));
		var bytes = track.Pcm.Length * 2;
		writer.Write("RIFF"u8); writer.Write(36 + bytes); writer.Write("WAVEfmt "u8); writer.Write(16);
		writer.Write((short)1); writer.Write((short)track.Channels); writer.Write(track.Rate);
		writer.Write(track.Rate * track.Channels * 2); writer.Write((short)(track.Channels * 2)); writer.Write((short)16);
		writer.Write("data"u8); writer.Write(bytes);
		foreach (var sample in track.Pcm) writer.Write(sample);
		return true;
	}

	private static Track? Decode(string path)
	{
		var file = File.ReadAllBytes(path);
		if (file.Length < 12 || BinaryPrimitives.ReadUInt32LittleEndian(file) != 0x46464952) return null; // "RIFF"
		ReadOnlySpan<byte> fmt = default, data = default;
		int samples = 0, delay = 0, loopStart = -1, loopEnd = -1;
		for (var offset = 12; offset + 8 <= file.Length;)
		{
			var id   = BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(offset));
			var size = (int)BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(offset + 4));
			if (size < 0 || offset + 8 + size > file.Length) break;
			var body = file.AsSpan(offset + 8, size);
			switch (id)
			{
				case 0x20746D66: fmt = body; break; // "fmt "
				case 0x61746164: data = body; break; // "data"
				case 0x74636166 when size >= 12: // "fact": total samples, overlap delay, encoder delay
					samples = BinaryPrimitives.ReadInt32LittleEndian(body);
					delay   = BinaryPrimitives.ReadInt32LittleEndian(body[8..]);
					break;
				case 0x6C706D73 when size >= 60 && BinaryPrimitives.ReadInt32LittleEndian(body[28..]) > 0: // "smpl"
					loopStart = BinaryPrimitives.ReadInt32LittleEndian(body[44..]);
					loopEnd   = BinaryPrimitives.ReadInt32LittleEndian(body[48..]);
					break;
			}
			offset += 8 + size + (size & 1);
		}
		if (fmt.Length < 48 || data.IsEmpty) return null;

		var handle = Atrac9.GetHandle();
		try
		{
			var config = fmt.Slice(44, 4).ToArray();
			if (Atrac9.InitDecoder(handle, config) != 0) return null;
			Atrac9.CodecInfo info;
			Atrac9.GetCodecInfo(handle, &info);
			if (info.channels <= 0 || info.frameSamples <= 0 || info.superframeSize <= 0) return null;
			var superframes = data.Length / info.superframeSize;
			var pcm         = new short[(long)superframes * info.framesInSuperframe * info.frameSamples * info.channels];
			var written     = 0;
			fixed (byte* input = data)
			fixed (short* output = pcm)
			{
				for (var s = 0; s < superframes; s++)
				{
					var position = input + (long)s * info.superframeSize;
					for (var f = 0; f < info.framesInSuperframe; f++)
					{
						int used;
						if (Atrac9.Decode(handle, position, output + written, &used, 0) != 0) break;
						position += used;
						written  += info.frameSamples * info.channels;
					}
				}
			}
			var total = written / info.channels;
			var start = Math.Clamp(delay, 0, total);
			var end   = samples > 0 ? Math.Min(total, start + samples) : total;
			var trimmed = pcm.AsSpan(start * info.channels, (end - start) * info.channels).ToArray();
			var length  = end - start;
			var from    = loopStart >= 0 && loopStart < length ? loopStart : 0;
			var to      = loopEnd > from && loopEnd < length ? loopEnd + 1 : length;
			return length > 0 ? new Track(trimmed, info.channels, info.samplingRate, from, to) : null;
		}
		finally
		{
			Atrac9.ReleaseHandle(handle);
		}
	}

	public void Dispose()
	{
		_running = false;
		_feeder.Join(500);
		lock (_lock) Close();
	}

	private static class Atrac9
	{
		[StructLayout(LayoutKind.Sequential)]
		public struct CodecInfo
		{
			public int channels, channelConfigIndex, samplingRate, superframeSize, framesInSuperframe, frameSamples, wlength;
			public uint configData;
		}

		[DllImport("atrac9", EntryPoint = "Atrac9GetHandle")]
		public static extern IntPtr GetHandle();

		[DllImport("atrac9", EntryPoint = "Atrac9ReleaseHandle")]
		public static extern void ReleaseHandle(IntPtr handle);

		[DllImport("atrac9", EntryPoint = "Atrac9InitDecoder")]
		public static extern int InitDecoder(IntPtr handle, byte[] config);

		[DllImport("atrac9", EntryPoint = "Atrac9Decode")]
		public static extern int Decode(IntPtr handle, byte* input, short* pcm, int* used, int noInterleave);

		[DllImport("atrac9", EntryPoint = "Atrac9GetCodecInfo")]
		public static extern int GetCodecInfo(IntPtr handle, CodecInfo* info);
	}
}
