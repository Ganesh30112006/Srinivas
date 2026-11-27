import { screen } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { DEVICE_IDS } from '@/test/fixtures/people';
import { renderApp } from '@/test/renderApp';

describe('Change Wi-Fi', () => {
  it('lets the lecturer send a new network to their recorder', async () => {
    const { user } = renderApp(`/devices/${DEVICE_IDS.room204}`, { as: 'priya' });
    expect(await screen.findByText('Campus-Staff')).toBeInTheDocument();
    await user.type(screen.getByLabelText('Network name'), 'Lab-2G');
    await user.type(screen.getByLabelText(/^Password/), 'short');
    await user.click(screen.getByRole('button', { name: 'Send to recorder' }));
    expect(await screen.findByText('Wi-Fi passwords have 8 to 63 characters.')).toBeInTheDocument();

    await user.type(screen.getByLabelText(/^Password/), '-enough');
    await user.click(screen.getByRole('button', { name: 'Send to recorder' }));
    expect(
      await screen.findByText('Sent. The recorder is restarting to join Lab-2G.'),
    ).toBeInTheDocument();
  });

  it('keeps students out of the device pages', async () => {
    renderApp(`/devices/${DEVICE_IDS.room204}`, { as: 'arjun' });
    expect(await screen.findByText("You don't have access to this page")).toBeInTheDocument();
    expect(screen.queryByLabelText('Network name')).not.toBeInTheDocument();
  });
});
