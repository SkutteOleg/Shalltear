namespace Shalltear;

public sealed partial class AudioSource {
	/// <summary>Play resource once over the current sound, at the source's volume.</summary>
	public void PlayOneShot(AudioResource resource) {
		PlayOneShot(resource, 1f);
	}
}
