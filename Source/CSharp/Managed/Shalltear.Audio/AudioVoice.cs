namespace Shalltear;

public readonly partial struct AudioVoice {
	/// <summary>False only for a voice never started.</summary>
	public bool IsValid => (_bits >> 32) != 0;
}
